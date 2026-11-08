#include "update/UpdateDownloadService.h"

#include "update/ApplicationVersion.h"
#include "update/UpdatePackage.h"

#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPromise>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <exception>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
using State = UpdateDownloadState;
using Result = UpdateDownloadResult;
constexpr qint64 ChecksumLimit = 65536;
constexpr qint64 BufferSize = 128 * 1024;
const QString CacheOwner = QStringLiteral("Arcanblade.Update.Download.v1");

bool isBusyState(State state)
{
    return state == State::Downloading || state == State::Preparing;
}

bool safePath(QString path)
{
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null)) return false;
    path = QDir::cleanPath(path);
    for (;;) {
        const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction()) return false;
#ifdef Q_OS_WIN
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
#endif
        const auto parent = info.absolutePath();
        if (parent == path) return true;
        path = parent;
    }
}

bool loopbackUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != QStringLiteral("http") || url.isRelative()
        || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    QHostAddress host;
    return url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
        || (host.setAddress(url.host()) && host.isLoopback());
}

QString cacheRecord(const QString &root, const QString &version)
{
    return QDir(root).filePath(QStringLiteral("ready-") + version + QStringLiteral(".json"));
}

bool sessionNameValid(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^download-[0-9a-f-]{36}$"));
    return expression.match(name).hasMatch();
}

QString cachedSessionName(const QString &root, const QString &version)
{
    const auto path = cacheRecord(root, version);
    QFile file(path);
    if (!safePath(path) || !file.open(QIODevice::ReadOnly) || file.size() > ChecksumLimit) return {};
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    const auto session = record.value(QStringLiteral("session")).toString();
    return record.value(QStringLiteral("owner")).toString() == CacheOwner
        && sessionNameValid(session) ? session : QString();
}

bool removeOwnedSession(const QString &root, const QString &name)
{
    if (!sessionNameValid(name) || !safePath(root)) return false;
    const auto directory = QDir(root).filePath(name);
    if (!safePath(directory) || !QFileInfo(directory).isDir()) return false;
    if (QFileInfo::exists(QDir(directory).filePath(QStringLiteral("prepared/.install-lease"))))
        return false;
    QFile owner(QDir(directory).filePath(QStringLiteral(".owner")));
    if (!safePath(owner.fileName()) || !owner.open(QIODevice::ReadOnly) || owner.size() > 128
        || owner.readAll() != CacheOwner.toUtf8()) return false;
    owner.close();
    QDirIterator entries(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDirIterator::Subdirectories);
    while (entries.hasNext()) if (!safePath(entries.next())) return false;
    // The absolute child path, its unique generated name, owner marker and all
    // descendants were checked; nothing outside this session is removed.
    if (!QDir(directory).removeRecursively()) return false;
    const auto records = QDir(root).entryInfoList({QStringLiteral("ready-*.json")}, QDir::Files);
    for (const auto &record : records) {
        QFile file(record.absoluteFilePath());
        if (!safePath(file.fileName()) || !file.open(QIODevice::ReadOnly) || file.size() > ChecksumLimit) continue;
        const auto value = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (value.value(QStringLiteral("owner")).toString() == CacheOwner
            && value.value(QStringLiteral("session")).toString() == name) QFile::remove(file.fileName());
    }
    return true;
}

void pruneOldSessions(const QString &root, const QString &keep)
{
    if (!safePath(root)) return;
    const auto directories = QDir(root).entryInfoList({QStringLiteral("download-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &directory : directories) {
        const auto name = directory.fileName();
        if (name == keep || !sessionNameValid(name) || !safePath(directory.absoluteFilePath())) continue;
        QLockFile lease(QDir(directory.absoluteFilePath()).filePath(QStringLiteral(".lease")));
        if (!lease.tryLock(0)) continue; // Another instance still uses its package.
        lease.unlock();
        removeOwnedSession(root, name);
    }
}

QByteArray cachedArchiveHash(const QString &path, const std::atomic_bool &cancelled)
{
    QFile file(path);
    if (!safePath(path) || !file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled.load()) return {};
        const auto bytes = file.read(1024 * 1024);
        if (bytes.isEmpty()) return {};
        digest.addData(bytes);
    }
    return digest.result().toHex();
}

QString checksumHash(const QByteArray &body, const QString &fileName)
{
    if (body.isEmpty() || body.size() > ChecksumLimit || body.contains('\0')) return {};
    const auto text = QString::fromUtf8(body).trimmed();
    static const QRegularExpression expression(QStringLiteral("^([0-9A-Fa-f]{64}) [ *]([^\\r\\n]+)$"));
    const auto match = expression.match(text);
    if (!match.hasMatch() || match.captured(2) != fileName) return {};
    return match.captured(1).toLower();
}

Result failure(const UpdateRelease &release, const QString &message, bool cancelled = false)
{
    Result result;
    result.release = release;
    result.state = cancelled ? State::Cancelled : State::Failed;
    result.message = cancelled ? QStringLiteral("已取消下载更新。") : message;
    return result;
}

bool writeReady(const QString &root, const QString &sessionName, const UpdateRelease &release,
                const QString &hash, const QByteArray &manifestHash, QString *error)
{
    const auto path = cacheRecord(root, release.version);
    if (!safePath(path)) {
        *error = QStringLiteral("更新缓存记录路径无效。");
        return false;
    }
    const auto bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("owner"), CacheOwner}, {QStringLiteral("version"), release.version},
        {QStringLiteral("session"), sessionName},
        {QStringLiteral("packageUrl"), release.packageUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("packageSize"), double(release.packageSize)},
        {QStringLiteral("sha256"), hash},
        {QStringLiteral("preparedManifestSha256"), QString::fromLatin1(manifestHash)}}).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = QStringLiteral("无法保存已下载更新的记录：%1").arg(file.errorString());
        return false;
    }
    return true;
}

Result prepareDownloaded(const QString &root, const QString &sessionName,
                         const UpdateRelease &release, const QString &hash,
                         const std::atomic_bool &cancelled)
{
    const auto session = QDir(root).filePath(sessionName);
    const auto package = QDir(session).filePath(QStringLiteral("package.zip"));
    if (!safePath(session) || !safePath(package))
        return failure(release, QStringLiteral("更新缓存目录不安全，未安装任何文件。"));
    const UpdatePackageRequest request{package, release.version, hash.toLatin1(),
        QDir(session).filePath(QStringLiteral("prepared"))};
    const auto prepared = prepareUpdatePackage(request, cancelled);
    if (cancelled.load()) return failure(release, {}, true);
    if (!prepared.success()) return failure(release, prepared.error);
    const auto manifestHash = cachedArchiveHash(
        QDir(prepared.rootDirectory).filePath(QStringLiteral("FILES.json")), cancelled);
    if (cancelled.load()) return failure(release, {}, true);
    if (manifestHash.size() != 64)
        return failure(release, QStringLiteral("无法保存已校验更新包的文件清单摘要，请重新下载。"));
    QString error;
    if (!writeReady(root, sessionName, release, hash, manifestHash, &error)) return failure(release, error);
    Result result;
    result.state = State::Ready;
    result.release = release;
    result.packagePath = package;
    result.preparedDirectory = prepared.rootDirectory;
    result.preparedManifestSha256 = manifestHash;
    result.message = QStringLiteral("更新已下载并校验完成，可以重启安装。");
    return result;
}

Result restoreDownloaded(const QString &root, const UpdateRelease &release,
                         const std::atomic_bool &cancelled)
{
    const auto path = cacheRecord(root, release.version);
    QFile file(path);
    if (!safePath(path) || !file.open(QIODevice::ReadOnly) || file.size() > ChecksumLimit)
        return failure(release, QStringLiteral("无法读取更新缓存，请重新下载。"));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    const auto record = document.object();
    const auto sessionName = record.value(QStringLiteral("session")).toString();
    const auto hash = record.value(QStringLiteral("sha256")).toString();
    const auto manifestHash = record.value(QStringLiteral("preparedManifestSha256")).toString();
    static const QRegularExpression hashExpression(QStringLiteral("^[0-9a-f]{64}$"));
    if (parse.error != QJsonParseError::NoError || !document.isObject()
        || record.value(QStringLiteral("owner")).toString() != CacheOwner
        || record.value(QStringLiteral("version")).toString() != release.version
        || record.value(QStringLiteral("packageUrl")).toString() != release.packageUrl.toString(QUrl::FullyEncoded)
        || record.value(QStringLiteral("packageSize")).toDouble() != double(release.packageSize)
        || !sessionNameValid(sessionName) || !hashExpression.match(hash).hasMatch()
        || !hashExpression.match(manifestHash).hasMatch()
        || (!release.packageSha256.isEmpty() && release.packageSha256.compare(hash, Qt::CaseInsensitive) != 0))
        return failure(release, QStringLiteral("更新缓存记录与发布信息不一致，请重新下载。"));
    const auto session = QDir(root).filePath(sessionName);
    const auto package = QDir(session).filePath(QStringLiteral("package.zip"));
    if (!safePath(package) || !QFileInfo(package).isFile() || QFileInfo(package).size() != release.packageSize)
        return failure(release, QStringLiteral("更新缓存文件不完整，请重新下载。"));
    if (cachedArchiveHash(package, cancelled) != hash.toLatin1())
        return failure(release, QStringLiteral("更新缓存摘要校验失败，请重新下载。"), cancelled.load());
    // The fixed child path is derived from the trusted release version; cached
    // absolute paths are never accepted. Reuse it instead of extracting a new
    // Qt tree on every check when the user postpones installation.
    const auto preparedRoot = QDir(session).filePath(
        QStringLiteral("prepared/Arcanblade-") + release.version + QStringLiteral("-windows-x64"));
    const auto manifestPath = QDir(preparedRoot).filePath(QStringLiteral("FILES.json"));
    if (cachedArchiveHash(manifestPath, cancelled) != manifestHash.toLatin1())
        return failure(release, QStringLiteral("更新缓存文件清单与原下载包不一致，请重新下载。"), cancelled.load());
    const auto prepared = validatePreparedUpdatePackage(preparedRoot, release.version, cancelled);
    if (cancelled.load()) return failure(release, {}, true);
    if (!prepared.success()) return failure(release, prepared.error);
    if (cachedArchiveHash(manifestPath, cancelled) != manifestHash.toLatin1())
        return failure(release, QStringLiteral("更新缓存文件清单在校验期间发生变化，请重新下载。"), cancelled.load());
    Result result;
    result.state = State::Ready;
    result.release = release;
    result.packagePath = package;
    result.preparedDirectory = prepared.rootDirectory;
    result.preparedManifestSha256 = manifestHash.toLatin1();
    result.message = QStringLiteral("已重新校验上次下载的更新，可以重启安装。");
    return result;
}
} // namespace

struct UpdateDownloadService::Private {
    explicit Private(UpdateDownloadService *owner, UpdateDownloadOptions supplied)
        : q(owner), options(std::move(supplied)), manager(new QNetworkAccessManager(owner)),
          timer(new QTimer(owner))
    {
        if (options.cacheRoot.isEmpty()) {
            const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (!data.isEmpty()) options.cacheRoot = QDir(data).filePath(QStringLiteral("updates"));
        }
        if (!options.loopbackEndpoint.isEmpty() && loopbackUrl(options.loopbackEndpoint))
            manager->setProxy(QNetworkProxy::NoProxy);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, q, [this] {
            finish(failure(release, QStringLiteral("下载更新超时，请检查网络或代理后重试。")));
        });
    }

    UpdateDownloadService *q;
    UpdateDownloadOptions options;
    QNetworkAccessManager *manager;
    QTimer *timer;
    QPointer<QNetworkReply> reply;
    std::unique_ptr<QSaveFile> output;
    std::shared_ptr<QLockFile> lease;
    QByteArray checksum;
    QString hash;
    QString sessionName;
    UpdateRelease release;
    std::shared_ptr<std::atomic_bool> cancelled;
    quint64 generation = 0;
    qint64 received = 0;
    qint64 ignoredResponseBytes = 0;
    int redirects = 0;
    bool fetchingChecksum = true;
    bool shuttingDown = false;
    bool shutdownEmitted = false;
    bool completionPending = false;
    Result pendingResult;
    bool workerActive = false;
    quint64 publicationRevision = 0;

    bool validOptions() const
    {
        return !options.cacheRoot.isEmpty() && safePath(options.cacheRoot)
            && options.timeoutMs > 0 && options.timeoutMs <= 2 * 60 * 60 * 1000
            && options.maximumPackageBytes > 0 && options.maximumPackageBytes <= 2LL * 1024 * 1024 * 1024
            && (options.loopbackEndpoint.isEmpty()
                || (loopbackUrl(options.loopbackEndpoint) && !options.loopbackEndpoint.hasQuery()));
    }

    void stopNetwork()
    {
        timer->stop();
        if (reply) {
            auto *old = reply.data();
            reply.clear();
            QObject::disconnect(old, nullptr, q, nullptr);
            if (!old->isFinished()) old->abort();
            old->deleteLater();
        }
        if (output) { output->cancelWriting(); output.reset(); }
    }

    void maybeShutdown()
    {
        if (!shuttingDown || shutdownEmitted || workerActive || q->isBusy() || completionPending) return;
        shutdownEmitted = true;
        QTimer::singleShot(0, q, [this] { emit q->shutdownFinished(); });
    }

    void finish(Result result)
    {
        if (completionPending) return;
        completionPending = true;
        pendingResult = std::move(result);
        stopNetwork();
        if (pendingResult.state != State::Ready && !workerActive) {
            lease.reset();
            // Network failures have only a ZIP/owner/lease, while preparation
            // failures have already cleaned their tree on the worker below.
            if (q->state() == State::Downloading)
                removeOwnedSession(options.cacheRoot, sessionName);
        }
        const auto id = generation;
        QTimer::singleShot(0, q, [this, id] {
            if (id != generation || !completionPending) return;
            completionPending = false;
            const QPointer<UpdateDownloadService> guard(q);
            q->publishResult(pendingResult);
            if (guard) maybeShutdown();
        });
    }

    QUrl transportUrl(const QUrl &original) const
    {
        if (options.loopbackEndpoint.isEmpty()) return original;
        QUrl mapped(options.loopbackEndpoint);
        mapped.setPath(original.path());
        mapped.setQuery(original.query());
        return mapped;
    }

    bool allowedRedirect(const QUrl &url) const
    {
        if (!options.loopbackEndpoint.isEmpty())
            return loopbackUrl(url) && url.host() == options.loopbackEndpoint.host()
                && url.port() == options.loopbackEndpoint.port();
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.isRelative()
            || !url.userInfo().isEmpty() || url.hasFragment()
            || (url.port() != -1 && url.port() != 443)) return false;
        if (url.host() == QStringLiteral("github.com"))
            return url == (fetchingChecksum ? release.checksumUrl : release.packageUrl);
        return url.host() == QStringLiteral("release-assets.githubusercontent.com")
            || url.host() == QStringLiteral("objects.githubusercontent.com")
            || url.host() == QStringLiteral("github-releases.githubusercontent.com");
    }

    bool current(QNetworkReply *value, quint64 id) const
    {
        return q->isBusy() && !completionPending && generation == id && reply == value;
    }

    void read(QNetworkReply *value, quint64 id)
    {
        if (!current(value, id)) return;
        const auto bytes = value->read(BufferSize);
        const int status = value->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (bytes.isEmpty() && value->bytesAvailable() > 0) {
            finish(failure(release, QStringLiteral("无法读取更新下载响应，请重试。")));
            return;
        }
        if (status != 200) {
            ignoredResponseBytes += bytes.size();
            if (ignoredResponseBytes > ChecksumLimit)
                finish(failure(release, QStringLiteral("更新服务器返回的错误响应过大。")));
            return;
        }
        received += bytes.size();
        const qint64 maximum = fetchingChecksum ? ChecksumLimit : release.packageSize;
        if (received > maximum) {
            finish(failure(release, QStringLiteral("更新下载超出声明大小，已停止下载。")));
            return;
        }
        if (fetchingChecksum) checksum += bytes;
        else if (!output || output->write(bytes) != bytes.size()) {
            finish(failure(release, QStringLiteral("无法写入更新文件，请检查磁盘空间和权限。")));
            return;
        }
        if (!fetchingChecksum) q->reportProgress(received, release.packageSize);
    }

    void request(const QUrl &url, quint64 id)
    {
        if (!q->isBusy() || completionPending || generation != id) return;
        ignoredResponseBytes = 0;
        QNetworkRequest networkRequest(url);
        networkRequest.setRawHeader("User-Agent", QByteArray("Arcanblade/") + applicationBuildVersion().toLatin1());
        networkRequest.setRawHeader("Accept", "application/octet-stream");
        networkRequest.setRawHeader("Accept-Encoding", "identity");
        networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        networkRequest.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        networkRequest.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        networkRequest.setTransferTimeout(60000);
        auto *active = manager->get(networkRequest);
        reply = active;
        active->setReadBufferSize(BufferSize);
        QObject::connect(active, &QNetworkReply::metaDataChanged, q, [this, active, id] {
            if (!current(active, id)) return;
            const int status = active->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 200) return;
            bool ok = false;
            const auto length = active->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
            if ((ok && (length < 0 || length > (fetchingChecksum ? ChecksumLimit : release.packageSize)))
                || (!fetchingChecksum && ok && length != release.packageSize)
                || (!active->rawHeader("Content-Encoding").isEmpty()
                    && active->rawHeader("Content-Encoding").toLower() != "identity"))
                finish(failure(release, QStringLiteral("更新服务器返回的大小或编码无效。")));
        });
        QObject::connect(active, &QIODevice::readyRead, q, [this, active, id] {
            // Bounded chunks keep network/disk buffers bounded without storing
            // an entire package in the GUI process heap.
            const QPointer<UpdateDownloadService> guard(q);
            while (current(active, id) && active->bytesAvailable() > 0) {
                read(active, id);
                if (!guard) return;
            }
        });
        QObject::connect(active, &QNetworkReply::finished, q, [this, active, id] {
            if (!current(active, id)) return;
            const QPointer<UpdateDownloadService> guard(q);
            while (current(active, id) && active->bytesAvailable() > 0) {
                read(active, id);
                if (!guard) return;
            }
            if (!current(active, id)) return;
            const int status = active->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400) {
                const auto target = active->url().resolved(active->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
                if (++redirects > 5 || !allowedRedirect(target)) {
                    finish(failure(release, QStringLiteral("更新下载跳转到不受支持的地址，已停止下载。")));
                    return;
                }
                reply.clear();
                QObject::disconnect(active, nullptr, q, nullptr);
                active->deleteLater();
                received = 0;
                checksum.clear();
                QTimer::singleShot(0, q, [this, target, id] { request(target, id); });
                return;
            }
            if (active->error() != QNetworkReply::NoError || status != 200) {
                finish(failure(release, QStringLiteral("无法下载更新，请检查网络、代理或证书后重试，也可前往 GitHub 下载。")));
                return;
            }
            reply.clear();
            QObject::disconnect(active, nullptr, q, nullptr);
            active->deleteLater();
            if (fetchingChecksum) {
                hash = checksumHash(checksum, release.packageUrl.path(QUrl::FullyDecoded).section(QLatin1Char('/'), -1));
                if (hash.isEmpty() || (!release.packageSha256.isEmpty()
                    && release.packageSha256.compare(hash, Qt::CaseInsensitive) != 0)) {
                    finish(failure(release, QStringLiteral("更新校验文件无效或与发布摘要不一致。")));
                    return;
                }
                fetchingChecksum = false;
                received = 0;
                redirects = 0;
                output = std::make_unique<QSaveFile>(QDir(QDir(options.cacheRoot).filePath(sessionName)).filePath(QStringLiteral("package.zip")));
                if (!output->open(QIODevice::WriteOnly)) {
                    finish(failure(release, QStringLiteral("无法创建更新下载文件，请检查目录权限。")));
                    return;
                }
                q->reportProgress(0, release.packageSize);
                if (!guard || !q->isBusy() || completionPending || generation != id) return;
                QTimer::singleShot(0, q, [this, id] { request(transportUrl(release.packageUrl), id); });
            } else {
                if (received != release.packageSize || !output || !output->commit()) {
                    finish(failure(release, QStringLiteral("更新文件未完整下载或无法保存，请重试。")));
                    return;
                }
                output.reset();
                timer->stop();
                prepare(false);
            }
        });
    }

    void prepare(bool restoring)
    {
        workerActive = true;
        const auto id = generation;
        const auto workerCancel = cancelled;
        const auto workerRelease = release;
        const auto workerRoot = options.cacheRoot;
        const auto workerSession = sessionName;
        const auto workerHash = hash;
        const auto workerLease = lease;
        auto promise = std::make_shared<QPromise<Result>>();
        auto *watcher = new QFutureWatcher<Result>(q);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, q, [this, watcher, id] {
            const auto result = watcher->result();
            watcher->deleteLater();
            workerActive = false;
            if (id != generation) { maybeShutdown(); return; }
            finish(result);
        });
        watcher->setFuture(promise->future());
        QThreadPool::globalInstance()->start([promise, workerCancel, workerRelease,
            workerRoot, workerSession, workerHash, workerLease, restoring] {
            promise->start();
            Result result;
            try {
                result = restoring ? restoreDownloaded(workerRoot, workerRelease, *workerCancel)
                    : prepareDownloaded(workerRoot, workerSession, workerRelease, workerHash, *workerCancel);
            } catch (const std::exception &exception) {
                result = failure(workerRelease, QStringLiteral("准备更新失败：%1").arg(QString::fromUtf8(exception.what())));
            } catch (...) {
                result = failure(workerRelease, QStringLiteral("准备更新时发生未知错误。"));
            }
            if (workerCancel->load()) result = failure(workerRelease, {}, true);
            if (result.state != State::Ready) {
                if (workerLease) workerLease->unlock();
                if (!restoring || !workerCancel->load())
                    removeOwnedSession(workerRoot, workerSession);
            } else pruneOldSessions(workerRoot, workerSession);
            promise->addResult(result);
            promise->finish();
        });
        Result preparing;
        preparing.state = State::Preparing;
        preparing.release = release;
        preparing.message = restoring ? QStringLiteral("正在重新校验已下载的更新…")
            : QStringLiteral("正在校验并准备更新文件…");
        q->publishResult(preparing);
    }
};

UpdateDownloadService::UpdateDownloadService(QObject *parent)
    : UpdateDownloadService(UpdateDownloadOptions{}, parent)
{
}

UpdateDownloadService::UpdateDownloadService(const UpdateDownloadOptions &options, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, options))
{
    qRegisterMetaType<UpdateDownloadState>();
    qRegisterMetaType<UpdateDownloadResult>();
}

UpdateDownloadService::~UpdateDownloadService()
{
    if (d->cancelled) d->cancelled->store(true);
    d->stopNetwork();
}

bool UpdateDownloadService::isBusy() const { return isBusyState(m_result.state); }

bool UpdateDownloadService::start(const UpdateRelease &release)
{
    if (isBusy() || d->completionPending || d->shuttingDown) return false;
    ++d->generation;
    d->release = release;
    d->lease.reset();
    d->cancelled = std::make_shared<std::atomic_bool>(false);
    d->checksum.clear();
    d->hash.clear();
    d->received = 0;
    d->redirects = 0;
    d->fetchingChecksum = true;
    d->sessionName = QStringLiteral("download-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto id = d->generation;
    Result downloading;
    downloading.state = State::Downloading;
    downloading.release = release;
    downloading.message = QStringLiteral("正在获取更新校验信息…");
    QPointer<UpdateDownloadService> guard(this);
    publishResult(downloading);
    if (!guard || !isBusy() || d->generation != id || d->completionPending) return true;
    if (!isDownloadableUpdateRelease(release) || !d->validOptions()
        || release.packageSize > d->options.maximumPackageBytes) {
        d->finish(failure(release, QStringLiteral("此版本不支持软件内下载，请前往 GitHub 下载。")));
        return true;
    }
    const auto session = QDir(d->options.cacheRoot).filePath(d->sessionName);
    if (!safePath(session) || !QDir().mkpath(d->options.cacheRoot) || !QDir().mkdir(session)) {
        d->finish(failure(release, QStringLiteral("无法创建更新缓存目录，请检查权限。")));
        return true;
    }
    QFile owner(QDir(session).filePath(QStringLiteral(".owner")));
    const auto marker = CacheOwner.toUtf8();
    if (!owner.open(QIODevice::WriteOnly | QIODevice::NewOnly) || owner.write(marker) != marker.size()) {
        d->finish(failure(release, QStringLiteral("无法创建更新缓存标记。")));
        return true;
    }
    owner.close();
    d->lease = std::make_shared<QLockFile>(QDir(session).filePath(QStringLiteral(".lease")));
    if (!d->lease->tryLock(0)) {
        d->finish(failure(release, QStringLiteral("更新缓存正在被其他程序使用。")));
        return true;
    }
    d->timer->start(d->options.timeoutMs);
    QTimer::singleShot(0, this, [this, id] { d->request(d->transportUrl(d->release.checksumUrl), id); });
    return true;
}

bool UpdateDownloadService::restore(const UpdateRelease &release)
{
    if (isBusy() || d->completionPending || d->shuttingDown || !isDownloadableUpdateRelease(release)
        || !d->validOptions() || release.packageSize > d->options.maximumPackageBytes
        || !QFileInfo::exists(cacheRecord(d->options.cacheRoot, release.version))) return false;
    ++d->generation;
    d->release = release;
    d->lease.reset();
    d->sessionName = cachedSessionName(d->options.cacheRoot, release.version);
    if (d->sessionName.isEmpty()) return false;
    const auto directory = QDir(d->options.cacheRoot).filePath(d->sessionName);
    if (!safePath(directory)) return false;
    d->lease = std::make_shared<QLockFile>(QDir(directory).filePath(QStringLiteral(".lease")));
    if (!d->lease->tryLock(0)) { d->lease.reset(); return false; }
    d->cancelled = std::make_shared<std::atomic_bool>(false);
    d->prepare(true);
    return true;
}

void UpdateDownloadService::cancel()
{
    if (!isBusy()) return;
    if (d->cancelled) d->cancelled->store(true);
    if (d->completionPending) {
        d->pendingResult = failure(d->release, {}, true);
        return;
    }
    if (!d->workerActive) d->finish(failure(d->release, {}, true));
}

void UpdateDownloadService::shutdown()
{
    d->shuttingDown = true;
    cancel();
    d->maybeShutdown();
}

void UpdateDownloadService::publishResult(UpdateDownloadResult result)
{
    const auto revision = ++d->publicationRevision;
    const bool wasBusy = isBusy();
    const auto previousState = m_result.state;
    m_result = std::move(result);
    const bool busy = isBusy();
    const auto snapshot = m_result;
    QPointer<UpdateDownloadService> guard(this);
    if (previousState != snapshot.state) emit stateChanged(snapshot.state);
    if (!guard || d->publicationRevision != revision) return;
    // A state listener may synchronously cancel or replace a result.
    if (m_result.state != snapshot.state) return;
    if (wasBusy != busy) emit busyChanged(busy);
    if (!guard || d->publicationRevision != revision) return;
    if (snapshot.state == State::Ready || snapshot.state == State::Failed || snapshot.state == State::Cancelled)
        emit finished(snapshot);
}

void UpdateDownloadService::reportProgress(qint64 completedBytes, qint64 totalBytes)
{
    emit progressChanged(completedBytes, totalBytes);
}
} // namespace asmr
