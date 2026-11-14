#include "mobile/MobilePlaybackService.h"
#include "mobile/MobileOfflineZip.h"

#include "artwork/CoverArtLoader.h"
#include "library/LibraryRepository.h"
#include "library/NaturalSort.h"
#include "playback/AudioFileValidation.h"
#include "playback/LibrarySubtitleResolver.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>
#include <QThread>
#include <QUuid>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <mutex>

#define CPPHTTPLIB_HEADER_MAX_COUNT 64
#include <httplib.h>

static void initMobileResources()
{
    Q_INIT_RESOURCE(mobile_playback);
    Q_INIT_RESOURCE(mobile_playback_branding);
}

namespace asmr {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using httplib::Request;
using httplib::Response;

void json(Response &response, const QJsonObject &value)
{
    response.set_content(QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString(),
                         "application/json; charset=utf-8");
}

void fail(Response &response, int status, const QString &message)
{
    response.status = status;
    json(response, {{QStringLiteral("error"), message}});
}

qint64 id(const std::string &value)
{
    qint64 number = 0;
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    return result.ec == std::errc{} && result.ptr == end && number > 0 ? number : 0;
}

std::string sessionCookie(const Request &request)
{
    constexpr QByteArrayView name("arcanblade_session=");
    const auto cookies = QByteArray::fromStdString(request.get_header_value("Cookie")).split(';');
    for (const auto &cookie : cookies) {
        const auto item = cookie.trimmed();
        if (item.startsWith(name))
            return item.sliced(name.size()).toStdString();
    }
    return {};
}

bool openLibrary(LibraryRepository &repository, Response &response)
{
    if (repository.open().success)
        return true;
    fail(response, 503, QStringLiteral("作品库暂不可用，请稍后重试。"));
    return false;
}

void readAsset(Response &response, const QString &resource, const char *mime)
{
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(response, 404, QStringLiteral("页面资源不可用。"));
        return;
    }
    response.set_content(file.readAll().toStdString(), mime);
}

bool supportedAudio(const LibraryFile &file)
{
    return file.type == LibraryFileType::Audio && !file.missing
        && (file.extension.compare(QStringLiteral("mp3"), Qt::CaseInsensitive) == 0
            || file.extension.compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0);
}

QString resolvedPath(const QString &path)
{
    std::error_code error;
#ifdef Q_OS_WIN
    const auto canonical = std::filesystem::canonical(path.toStdWString(), error);
    return error ? QString{} : QDir::fromNativeSeparators(QString::fromStdWString(canonical.wstring()));
#else
    const auto canonical = std::filesystem::canonical(QFile::encodeName(path).constData(), error);
    return error ? QString{} : QFile::decodeName(canonical.c_str());
#endif
}

std::shared_ptr<QFile> openAudio(const QString &root, const LibraryFile &track)
{
    const auto first = validateLibraryAudioFile(root, track.relativePath);
    const auto path = first.success ? resolvedPath(first.path) : QString{};
    if (path.isEmpty()) return {};
    auto file = std::make_shared<QFile>(path);
    if (!file->open(QIODevice::ReadOnly) || file->size() <= 0) return {};
    const auto checked = validateLibraryAudioFile(root, track.relativePath);
    return checked.success && resolvedPath(checked.path) == path ? file : nullptr;
}

QList<LibraryFile> sortedTracks(const LibraryWorkSnapshotResult &snapshot)
{
    struct Track { LibraryFile file; NaturalSortKey key; };
    QList<Track> tracks;
    for (const auto &file : snapshot.files) {
        if (supportedAudio(file)) tracks.append({file, NaturalSortKey(file.relativePath)});
    }
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track &a, const Track &b) {
        return a.key.compare(b.key) < 0;
    });
    QList<LibraryFile> result;
    result.reserve(tracks.size());
    for (const auto &track : tracks) result.append(track.file);
    return result;
}

QJsonObject subtitlePayload(LibraryTrackSubtitlesResult snapshot)
{
    const auto selection = selectLibrarySubtitles(std::move(snapshot));
    QString state = QStringLiteral("none");
    QJsonArray entries;
    if (selection.decision == LibrarySubtitleDecision::Disabled) {
        state = QStringLiteral("disabled");
    } else if (selection.decision == LibrarySubtitleDecision::Error) {
        state = QStringLiteral("unavailable");
    } else if (selection.decision == LibrarySubtitleDecision::Selected) {
        const auto result = loadLibrarySubtitle(selection.snapshot.work.rootPath, selection.selectedFile);
        state = result.succeeded() ? QStringLiteral("ready") : QStringLiteral("unavailable");
        if (result.succeeded()) {
            for (const auto &entry : result.entries)
                entries.append(QJsonArray{entry.startMs, entry.endMs, entry.text});
        }
    }
    return {{QStringLiteral("state"), state}, {QStringLiteral("entries"), entries}};
}
} // namespace

struct MobilePlaybackService::Run {
    Run(ManualCoverSettings settings, QHostAddress localAddress, int prefix)
        : library(std::move(settings)), address(std::move(localAddress)), prefixLength(prefix) {}

    void stop()
    {
        cancelled->store(true);
        const std::lock_guard lock(serverMutex);
        if (listener) listener->stop();
    }

    void renew()
    {
        const std::lock_guard lock(sessionMutex);
        int next = QRandomGenerator::system()->bounded(lastCode < 0 ? 1000000 : 999999);
        if (lastCode >= 0 && next >= lastCode) ++next;
        lastCode = next;
        code = QString::number(next).rightJustified(6, u'0');
        codeExpires = Clock::now() + 5min;
    }

    PairingCodeInfo pairingCodeInfo()
    {
        const std::lock_guard lock(sessionMutex);
        if (code.isEmpty()) return {PairingCodeState::Used, {}};
        if (Clock::now() >= codeExpires) return {PairingCodeState::Expired, {}};
        return {PairingCodeState::Ready, code};
    }

    void pair(const Request &request, Response &response)
    {
        const auto origin = request.get_header_value("Origin");
        if ((!origin.empty() && origin != "http://" + host)
            || request.get_header_value("Sec-Fetch-Site") == "cross-site"
            || request.get_header_value("Content-Type") != "application/json" || request.body.size() > 1024) {
            fail(response, 403, QStringLiteral("请从手机播放页面配对。"));
            return;
        }
        const std::lock_guard lock(sessionMutex);
        const auto now = Clock::now();
        if (now - attemptWindow >= 1min) {
            attemptWindow = now;
            attempts = 0;
        }
        if (attempts >= 8) {
            fail(response, 429, QStringLiteral("尝试次数过多，请一分钟后重试。"));
            return;
        }
        ++attempts;
        const auto payload = QJsonDocument::fromJson(QByteArray::fromStdString(request.body));
        if (code.isEmpty() || now >= codeExpires
            || payload.object().value(QStringLiteral("code")).toString() != code) {
            fail(response, 401, QStringLiteral("配对码错误或已过期，请在电脑端获取新码。"));
            return;
        }
        code.clear();
        token = (QUuid::createUuid().toString(QUuid::Id128)
            + QUuid::createUuid().toString(QUuid::Id128)).toStdString();
        tokenExpires = now + 12h;
        response.set_header("Set-Cookie", "arcanblade_session=" + token + "; Path=/; HttpOnly; SameSite=Strict");
        json(response, {{QStringLiteral("paired"), true}});
    }

    bool authorized(const std::string &candidate)
    {
        const std::lock_guard lock(sessionMutex);
        const auto now = Clock::now();
        if (cancelled->load() || token.empty() || now >= tokenExpires || candidate != token)
            return false;
        tokenExpires = now + 12h;
        return true;
    }

    void works(Response &response)
    {
        LibraryRepository repository(library.databasePath, LibraryRepository::Access::ReadOnly);
        if (!openLibrary(repository, response)) return;
        const auto result = repository.works();
        if (!result.success) {
            fail(response, 503, QStringLiteral("无法读取作品列表。"));
            return;
        }
        QJsonArray items;
        for (const auto &work : result.works) {
            items.append(QJsonObject{{QStringLiteral("id"), QString::number(work.id)},
                                     {QStringLiteral("name"), work.displayName}});
        }
        json(response, {{QStringLiteral("works"), items}});
    }

    void tracks(qint64 workId, Response &response)
    {
        LibraryRepository repository(library.databasePath, LibraryRepository::Access::ReadOnly);
        if (!openLibrary(repository, response)) return;
        const auto snapshot = repository.workSnapshot(workId);
        if (!snapshot.success) {
            fail(response, 404, QStringLiteral("作品不可用，请刷新列表。"));
            return;
        }
        QJsonArray items;
        for (const auto &track : sortedTracks(snapshot)) {
            const auto file = openAudio(snapshot.work.rootPath, track);
            items.append(QJsonObject{{QStringLiteral("id"), QString::number(track.id)},
                                     {QStringLiteral("name"), track.fileName},
                                     {QStringLiteral("directory"), QFileInfo(track.relativePath).path()},
                                     {QStringLiteral("size"), file ? file->size() : 0}});
        }
        json(response, {{QStringLiteral("name"), snapshot.work.displayName}, {QStringLiteral("tracks"), items}});
    }

    void cover(qint64 workId, Response &response)
    {
        LibraryRepository repository(library.databasePath, LibraryRepository::Access::ReadOnly);
        if (!openLibrary(repository, response)) return;
        const auto snapshot = repository.workSnapshot(workId);
        if (!snapshot.success) {
            fail(response, 404, QStringLiteral("作品不可用。"));
            return;
        }
        workCover(snapshot.work, response);
    }

    void workCover(const LibraryWork &work, Response &response)
    {
        CoverArtRequest request;
        request.libraryWork = true;
        request.rootPath = work.rootPath;
        request.manualLookup = ManualCoverLookup{library, {work.id, work.rootPath, work.importedAtMs}, true};
        const auto image = CoverArtLoader::load(request, {}, cancelled);
        QBuffer bytes;
        if (image.status == CoverArtStatus::Ready && image.image.save(&bytes, "PNG")
            && !cancelled->load()) {
            response.set_content(bytes.data().toStdString(), "image/png");
            return;
        }
        readAsset(response, QStringLiteral(":/mobile-playback/arcanblade-mark-256.png"), "image/png");
    }

    void subtitles(qint64 workId, qint64 trackId, Response &response)
    {
        LibraryRepository repository(library.databasePath, LibraryRepository::Access::ReadOnly);
        if (!openLibrary(repository, response)) return;
        auto snapshot = repository.trackSubtitles(workId, trackId);
        if (!snapshot.success || !supportedAudio(snapshot.audio)) {
            fail(response, 404, QStringLiteral("音轨不可用。"));
            return;
        }
        repository.close();
        json(response, subtitlePayload(std::move(snapshot)));
    }

    void audio(const Request &request, qint64 workId, qint64 trackId, Response &response)
    {
        LibraryRepository repository(library.databasePath, LibraryRepository::Access::ReadOnly);
        if (!openLibrary(repository, response)) return;
        const auto snapshot = repository.workSnapshot(workId);
        if (!snapshot.success) {
            fail(response, 404, QStringLiteral("作品不可用。"));
            return;
        }
        const auto track = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(),
            [trackId](const LibraryFile &file) { return file.id == trackId && supportedAudio(file); });
        if (track == snapshot.files.cend()) {
            fail(response, 404, QStringLiteral("音轨不可用。"));
            return;
        }
        repository.close();
        auto file = openAudio(snapshot.work.rootPath, *track);
        if (!file) {
            fail(response, 404, QStringLiteral("音频文件不可读取。"));
            return;
        }
        const auto size = static_cast<size_t>(file->size());
        const auto mime = track->extension.compare(QStringLiteral("mp3"), Qt::CaseInsensitive) == 0
            ? "audio/mpeg" : "audio/wav";
        response.set_header("Accept-Ranges", "bytes");
        if (request.method == "HEAD") {
            response.status = 200;
            response.set_header("Content-Type", mime);
            response.set_header("Content-Length", std::to_string(size));
            return;
        }
        if (request.ranges.size() > 1 || request.get_header_value_count("Range") > 1) {
            response.set_header("Content-Range", "bytes */" + std::to_string(size));
            fail(response, 416, QStringLiteral("仅支持单段音频范围。"));
            return;
        }
        if (!request.ranges.empty()) {
            const auto [firstByte, lastByte] = request.ranges.front();
            if ((firstByte >= 0 && static_cast<size_t>(firstByte) >= size)
                || (firstByte == -1 && lastByte <= 0)
                || (firstByte >= 0 && lastByte >= 0 && lastByte < firstByte)) {
                response.set_header("Content-Range", "bytes */" + std::to_string(size));
                fail(response, 416, QStringLiteral("音频范围超出文件。"));
                return;
            }
        }
        auto credential = sessionCookie(request);
        response.set_content_provider(size, mime,
            [this, file, credential = std::move(credential)](size_t offset, size_t length, httplib::DataSink &sink) {
                if (!authorized(credential) || !sink.is_writable() || !file->seek(static_cast<qint64>(offset)))
                    return false;
                const auto bytes = file->read(static_cast<qint64>(std::min<size_t>(length, 64 * 1024)));
                return !bytes.isEmpty() && !cancelled->load()
                    && sink.write(bytes.constData(), static_cast<size_t>(bytes.size()));
            });
    }

    void offlinePackage(const Request &request, qint64 workId, Response &response)
    {
        if (request.get_header_value("Origin") != "http://" + host
            || request.get_header_value("Sec-Fetch-Site") == "cross-site"
            || request.get_header_value("Content-Type") != "application/x-www-form-urlencoded"
            || request.target.find('?') != std::string::npos || request.params.size() != 1
            || !request.has_param("tracks") || request.has_header("Range")) {
            fail(response, 400, QStringLiteral("请从当前作品页面选择要下载的音轨。"));
            return;
        }
        const auto values = QByteArray::fromStdString(request.get_param_value("tracks")).split(',');
        if (values.size() > 4096) {
            fail(response, 400, QStringLiteral("一次最多下载 4096 条音轨，请分次选择。"));
            return;
        }
        QSet<qint64> selected;
        for (const auto &value : values) {
            const auto number = id(value.toStdString());
            if (!number || selected.contains(number)) {
                fail(response, 400, QStringLiteral("音轨编号无效或重复。"));
                return;
            }
            selected.insert(number);
        }
        LibraryRepository repository(library.databasePath, LibraryRepository::Access::ReadOnly);
        if (!openLibrary(repository, response)) return;
        const auto snapshot = repository.workSnapshot(workId);
        if (!snapshot.success) {
            fail(response, 404, QStringLiteral("作品不可用，请刷新列表。"));
            return;
        }
        const auto work = snapshot.work;
        struct SelectedTrack { LibraryFile file; qsizetype order; };
        QList<SelectedTrack> files;
        qsizetype order = 0;
        for (const auto &file : sortedTracks(snapshot)) {
            if (selected.contains(file.id)) files.append({file, order});
            ++order;
        }
        if (files.size() != selected.size()) {
            fail(response, 404, QStringLiteral("所选音轨有缺失或不属于当前作品。"));
            return;
        }
        const auto credential = sessionCookie(request);
        for (const auto &[file, position] : files) {
            Q_UNUSED(position);
            if (!authorized(credential) || !openAudio(work.rootPath, file)) {
                fail(response, 409, QStringLiteral("所选音轨无法读取，请刷新后重试。"));
                return;
            }
        }
        const auto workKey = QCryptographicHash::hash(
            QJsonDocument(QJsonArray{work.rootPath, QString::number(work.importedAtMs), QString::number(work.id)})
                .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
        response.set_header("Content-Disposition", "attachment; filename=\"Arcanblade-offline.zip\"; filename*=UTF-8''"
            + QUrl::toPercentEncoding(work.displayName.left(100) + QStringLiteral("-离线包.zip")).toStdString());
        response.set_chunked_content_provider("application/zip",
            [this, work, workKey, files = std::move(files), credential](size_t, httplib::DataSink &sink) {
                const auto active = [&] { return sink.is_writable() && authorized(credential); };
                MobileOfflineZip zip([&](QByteArrayView bytes) {
                    return active() && sink.write(bytes.constData(), static_cast<size_t>(bytes.size()));
                });
                if (!active()) return false;
                Response cover;
                workCover(work, cover);
                if (cover.status >= 400 || !zip.add("cover.png", QByteArray::fromStdString(cover.body))) return false;
                LibraryRepository db(library.databasePath, LibraryRepository::Access::ReadOnly);
                if (!db.open().success) return false;
                QJsonArray tracks;
                for (const auto &[file, position] : files) {
                    if (!active()) return false;
                    auto subtitle = db.trackSubtitles(work.id, file.id);
                    if (!subtitle.success || subtitle.work != work || subtitle.audio != file) return false;
                    const auto suffix = file.extension.toLower().toLatin1();
                    const auto number = QByteArray::number(file.id);
                    const auto audioName = "audio-" + number + '.' + suffix;
                    const auto subtitleName = "subtitles-" + number + ".json";
                    const auto audioFile = openAudio(work.rootPath, file);
                    if (!audioFile || !zip.add(audioName, audioFile->size(), [&](qint64 size) {
                        return active() ? audioFile->read(size) : QByteArray{};
                    })) return false;
                    const auto cues = QJsonDocument(subtitlePayload(std::move(subtitle))).toJson(QJsonDocument::Compact);
                    if (!zip.add(subtitleName, cues)) return false;
                    tracks.append(QJsonObject{{QStringLiteral("id"), QString::number(file.id)},
                        {QStringLiteral("name"), file.fileName},
                        {QStringLiteral("order"), static_cast<qint64>(position)},
                        {QStringLiteral("audio"), QString::fromLatin1(audioName)},
                        {QStringLiteral("subtitles"), QString::fromLatin1(subtitleName)}});
                }
                const QJsonObject manifest{{QStringLiteral("format"), QStringLiteral("arcanblade-offline")},
                    {QStringLiteral("version"), 1}, {QStringLiteral("work_id"), QString::fromLatin1(workKey)},
                    {QStringLiteral("work_name"), work.displayName}, {QStringLiteral("tracks"), tracks}};
                if (!zip.add("manifest.json", QJsonDocument(manifest).toJson(QJsonDocument::Compact))
                    || !zip.finish()) return false;
                sink.done();
                return true;
            });
    }

    void routes(httplib::Server &server)
    {
        server.new_task_queue = [] { return new httplib::ThreadPool(6, 6, 12); };
        server.set_address_family(AF_INET).set_read_timeout(3).set_write_timeout(3)
            .set_keep_alive_max_count(1).set_payload_max_length(96 * 1024);
        server.set_default_headers({
            {"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
            {"Referrer-Policy", "no-referrer"},
            {"Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; "
                  "media-src 'self'; connect-src 'self'; "
                  "base-uri 'none'; frame-ancestors 'none'; form-action 'self'"}
        });
        server.set_pre_routing_handler([this](const Request &request, Response &response) {
            const QHostAddress peer(QString::fromStdString(request.remote_addr));
            if (cancelled->load() || request.get_header_value_count("Host") != 1
                || request.get_header_value("Host") != host
                || peer.protocol() != QAbstractSocket::IPv4Protocol
                || !peer.isInSubnet(address, prefixLength)) {
                fail(response, 403, QStringLiteral("此连接不在共享范围内。"));
            } else if (!request.path.starts_with("/api/") || request.path == "/api/pair"
                       || authorized(sessionCookie(request))) {
                return httplib::Server::HandlerResponse::Unhandled;
            } else {
                fail(response, 401, QStringLiteral("请先配对。"));
            }
            return httplib::Server::HandlerResponse::Handled;
        });
        server.Get("/", [](const Request &, Response &response) {
            // Form POSTs need a same-origin Origin header. Keep referrers off cross-origin links.
            response.headers.erase("Referrer-Policy");
            response.set_header("Referrer-Policy", "same-origin");
            readAsset(response, QStringLiteral(":/mobile-playback/index.html"), "text/html; charset=utf-8");
        });
        server.Get("/player.js", [](const Request &, Response &response) {
            readAsset(response, QStringLiteral(":/mobile-playback/player.js"), "text/javascript; charset=utf-8");
        });
        server.Get("/offline-transfer-sender.js", [](const Request &, Response &response) {
            readAsset(response, QStringLiteral(":/mobile-playback/offline-transfer-sender.js"),
                      "text/javascript; charset=utf-8");
        });
        server.Get("/player.css", [](const Request &, Response &response) {
            readAsset(response, QStringLiteral(":/mobile-playback/player.css"), "text/css; charset=utf-8");
        });
        server.Get("/mark.png", [](const Request &, Response &response) {
            readAsset(response, QStringLiteral(":/mobile-playback/arcanblade-mark-256.png"), "image/png");
        });
        server.Post("/api/pair", [this](const Request &request, Response &response) { pair(request, response); });
        server.Get("/api/works", [this](const Request &, Response &response) { works(response); });
        server.Get(R"(/api/works/([0-9]+)/tracks)", [this](const Request &request, Response &response) {
            tracks(id(request.matches[1].str()), response);
        });
        server.Post(R"(/api/works/([0-9]+)/offline-package)", [this](const Request &request, Response &response) {
            offlinePackage(request, id(request.matches[1].str()), response);
        });
        server.Get(R"(/api/works/([0-9]+)/cover)", [this](const Request &request, Response &response) {
            cover(id(request.matches[1].str()), response);
        });
        server.Get(R"(/api/works/([0-9]+)/tracks/([0-9]+)/subtitles)", [this](const Request &request, Response &response) {
            subtitles(id(request.matches[1].str()), id(request.matches[2].str()), response);
        });
        server.Get(R"(/api/works/([0-9]+)/tracks/([0-9]+)/audio)", [this](const Request &request, Response &response) {
            audio(request, id(request.matches[1].str()), id(request.matches[2].str()), response);
        });
    }

    ManualCoverSettings library;
    QHostAddress address;
    int prefixLength;
    std::string host;
    std::mutex serverMutex;
    std::unique_ptr<httplib::Server> listener;
    const std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    std::mutex sessionMutex;
    QString code;
    int lastCode = -1; // A refresh must invalidate the previous manual and QR link, even after use.
    Clock::time_point codeExpires{};
    Clock::time_point attemptWindow{};
    int attempts = 0;
    std::string token;
    Clock::time_point tokenExpires{};
    QString error; // Written by the worker and read after it exits.
};

MobilePlaybackService::MobilePlaybackService(ManualCoverSettings library, QObject *parent)
    : QObject(parent), m_library(std::move(library))
{
    static const bool initialized = [] { initMobileResources(); return true; }();
    Q_UNUSED(initialized);
}

MobilePlaybackService::~MobilePlaybackService()
{
    if (!m_worker) return;
    m_run->stop();
    m_worker->wait();
    delete m_worker;
}

void MobilePlaybackService::start(const QHostAddress &address, int prefixLength)
{
    if (!isIdle()) return;
    m_error.clear();
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address == QHostAddress::AnyIPv4
        || address == QHostAddress::Broadcast || address.isMulticast()
        || prefixLength <= 0 || prefixLength > 32) {
        m_error = QStringLiteral("请选择可用的 IPv4 网络接口。");
        emit changed();
        return;
    }
    const auto run = std::make_shared<Run>(m_library, address, prefixLength);
    m_run = run;
    m_state = State::Starting;
    m_worker = QThread::create([this, run] {
        try {
            const auto localIp = run->address.toString().toStdString();
            auto listener = std::make_unique<httplib::Server>();
            auto &server = *listener;
            run->routes(server);
            int port;
            {
                const std::lock_guard lock(run->serverMutex);
                if (run->cancelled->load()) return;
                run->listener = std::move(listener);
                port = server.bind_to_any_port(localIp);
            }
            if (port <= 0) {
                run->error = QStringLiteral("无法监听所选网络地址，请检查网络连接后重试。");
                return;
            }
            run->host = localIp + ":" + std::to_string(port);
            server.set_start_handler([this, runPtr = run.get()] {
                runPtr->renew();
                const auto url = QString::fromStdString("http://" + runPtr->host);
                QMetaObject::invokeMethod(this, [this, runPtr, url] {
                    if (m_state == State::Starting && m_run.get() == runPtr) {
                        m_address = url;
                        m_state = State::Listening;
                        emit changed();
                    }
                }, Qt::QueuedConnection);
            });
            if (!server.listen_after_bind() && !run->cancelled->load())
                run->error = QStringLiteral("手机播放服务已中断，请重新开启。");
        } catch (...) {
            run->error = QStringLiteral("无法启动手机播放服务。");
        }
        if (run->listener) run->listener->stop();
    });
    connect(m_worker, &QThread::finished, this, [this, run] {
        m_worker->deleteLater();
        m_worker = nullptr;
        m_error = run->cancelled->load() ? QString{} : run->error;
        m_run.reset();
        m_address.clear();
        m_state = State::Stopped;
        emit changed();
    });
    m_worker->start();
    emit changed();
}

void MobilePlaybackService::stop()
{
    if (isIdle() || m_state == State::Stopping) return;
    m_state = State::Stopping;
    m_run->stop();
    m_address.clear();
    emit changed();
}

void MobilePlaybackService::renewPairingCode()
{
    if (m_state == State::Listening) {
        m_run->renew();
        emit changed();
    }
}

MobilePlaybackService::PairingCodeInfo MobilePlaybackService::pairingCodeInfo() const
{
    return m_state == State::Listening ? m_run->pairingCodeInfo() : PairingCodeInfo{};
}

QString MobilePlaybackService::pairingCode() const
{
    return pairingCodeInfo().code;
}

} // namespace asmr
