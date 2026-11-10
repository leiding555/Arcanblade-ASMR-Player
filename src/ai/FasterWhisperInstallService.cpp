#include "ai/FasterWhisperInstallService.h"
#include "network/DownloadProxyResolver.h"
#include "ai/FasterWhisperArchive.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPromise>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
using Request = FasterWhisperInstallRequest;
using Result = FasterWhisperInstallResult;
using Progress = FasterWhisperInstallProgress;
using State = FasterWhisperInstallState;
using Report = std::function<void(const Progress &)>;
const QString Owner = QStringLiteral("Arcanblade.FasterWhisper.Install.v1");
const QString RootRecord = QStringLiteral(".arcanblade-xxl-install-root.json");
const QString InstallRecord = QStringLiteral(".arcanblade-xxl-install.json");

constexpr qint64 BufferSize = 1024 * 1024;

Result failure(const QString &error, const std::atomic_bool &cancelled)
{
    return {cancelled.load() ? State::Cancelled : State::Failed, {}, {},
        cancelled.load() ? QStringLiteral("Installation cancelled; resumable download was retained.") : error, {}};
}
bool reparsePoint(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymLink();
#endif
}
bool safeAncestors(QString path)
{
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        if (reparsePoint(path))
            return false;
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path)
            return true;
        path = parent;
    }
}
bool withinRoot(const QString &root, const QString &path)
{
    const QString base = QDir::cleanPath(QFileInfo(root).absoluteFilePath()) + QLatin1Char('/');
    const QString target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    return target.startsWith(base, Qt::CaseInsensitive);
#else
    return target.startsWith(base);
#endif
}
QJsonObject readRecord(const QString &path)
{
    QFile file(path);
    if (reparsePoint(path) || !file.open(QIODevice::ReadOnly) || file.size() > 16384)
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeRecord(const QString &path, const QString &fingerprint, QString *error)
{
    if (QFileInfo::exists(path) || reparsePoint(path)) {
        *error = QStringLiteral("Refusing to overwrite existing installation metadata: %1").arg(path);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("owner"), Owner},
        {QStringLiteral("fingerprint"), fingerprint}}).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
bool recordMatches(const QString &path, const QString &fingerprint)
{
    const auto record = readRecord(path);
    return record.value(QStringLiteral("owner")).toString() == Owner
        && record.value(QStringLiteral("fingerprint")).toString() == fingerprint;
}
QString fingerprint(const QString &version, const QList<FasterWhisperDownloadFile> &files)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(version.toUtf8());
    for (const auto &file : files) {
        hash.addData(file.name.toUtf8());
        hash.addData(QByteArray::number(file.size));
        hash.addData(file.sha256);
    }
    return QString::fromLatin1(hash.result().toHex());
}
QByteArray fileHash(QFile &file, qint64 start, qint64 length,
    const std::atomic_bool &cancelled, QString *error)
{
    if (!file.seek(start)) {
        *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (length > 0 && !cancelled.load()) {
        const QByteArray block = file.read(std::min(length, BufferSize));
        if (block.isEmpty()) {
            *error = QStringLiteral("Could not read complete cached data: %1").arg(file.errorString());
            return {};
        }
        hash.addData(block);
        length -= block.size();
    }
    return cancelled.load() ? QByteArray{} : hash.result().toHex();
}
bool loopback(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1")
        || host == QStringLiteral("localhost");
}
bool urlAllowed(const QUrl &url, bool allowLoopback)
{
    return url.scheme() == QStringLiteral("https")
        || (allowLoopback && url.scheme() == QStringLiteral("http") && loopback(url));
}
struct TransferResult { QString error; bool retryable = false; };

TransferResult transfer(QNetworkAccessManager &manager, QFile &archive,
    const FasterWhisperDownloadFile &part, qint64 base, const Request &request,
    const std::atomic_bool &cancelled, const Report &report,
    qint64 progressBase, qint64 progressTotal, const QString &component)
{
    const qint64 offset = std::clamp(archive.size() - base, qint64(0), part.size);
    QNetworkRequest query(part.url);
    query.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    query.setRawHeader("Accept-Encoding", "identity");
    if (offset > 0)
        query.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
    QNetworkReply *reply = manager.get(query);
    reply->setReadBufferSize(BufferSize);
    QEventLoop eventLoop;
    QTimer monitor;
    QElapsedTimer activity;
    activity.start();
    TransferResult result;
    bool accepted = false, inspected = false;
    qint64 expected = 0, written = 0;
    int redirects = 0;
    auto reject = [&](const QString &error, bool retryable = false) {
        if (result.error.isEmpty()) {
            result.error = error;
            result.retryable = retryable;
        }
        reply->abort();
    };
    auto headers = [&] {
        if (inspected || !reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid())
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400)
            return;
        inspected = true;
        if (status == 206) {
            static const QRegularExpression range(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$"));
            const auto match = range.match(QString::fromLatin1(reply->rawHeader("Content-Range")));
            bool a = false, b = false, c = false;
            const qint64 start = match.captured(1).toLongLong(&a);
            const qint64 end = match.captured(2).toLongLong(&b);
            const qint64 total = match.captured(3).toLongLong(&c);
            if (!match.hasMatch() || !a || !b || !c || start != offset || end < start
                || end >= part.size || total != part.size) {
                reject(QStringLiteral("Invalid HTTP Content-Range for %1.").arg(part.name));
                return;
            }
            expected = end - start + 1;
            if (!archive.seek(base + offset)) {
                reject(archive.errorString());
                return;
            }
        } else if (status == 200) {
            // An ignored Range restarts this fragment, never an earlier verified fragment.
            if (!archive.resize(base) || !archive.seek(base)) {
                reject(archive.errorString());
                return;
            }
            expected = part.size;
        } else {
            reject(QStringLiteral("HTTP %1 while downloading %2.").arg(status).arg(part.name),
                status == 408 || status == 429 || status >= 500 || status == 416);
            return;
        }
        const QByteArray lengthHeader = reply->rawHeader("Content-Length");
        if (!lengthHeader.isEmpty()) {
            bool ok = false;
            const qint64 length = lengthHeader.toLongLong(&ok);
            if (!ok || length != expected) {
                reject(QStringLiteral("Unexpected HTTP Content-Length for %1.").arg(part.name));
                return;
            }
        }
        accepted = true;
        activity.restart();
    };
    auto consume = [&] {
        headers();
        if (!accepted || !result.error.isEmpty())
            return;
        while (reply->bytesAvailable() > 0 && !cancelled.load()) {
            const QByteArray block = reply->read(BufferSize);
            if (written > expected - block.size()) {
                reject(QStringLiteral("Download exceeded the declared fragment size."));
                return;
            }
            if (archive.write(block) != block.size()) {
                reject(archive.errorString());
                return;
            }
            written += block.size();
            activity.restart();
            report({State::Downloading, progressBase + archive.pos(), progressTotal,
                QStringLiteral("Downloading %1").arg(part.name), component});
        }
    };
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &eventLoop, headers);
    QObject::connect(reply, &QNetworkReply::readyRead, &eventLoop, consume);
    QObject::connect(reply, &QNetworkReply::redirected, &eventLoop, [&](const QUrl &url) {
        if (++redirects > 8 || !urlAllowed(url, request.allowLoopbackHttp)) {
            reject(QStringLiteral("Download redirect was rejected."));
            return;
        }
        activity.restart();
        reply->redirectAllowed();
    });
    QObject::connect(reply, &QNetworkReply::finished, &eventLoop, &QEventLoop::quit);
    QObject::connect(&monitor, &QTimer::timeout, &eventLoop, [&] {
        if (cancelled.load())
            reject(QStringLiteral("Download cancelled."));
        else if (activity.elapsed() >= std::clamp(request.networkTimeoutMs, 100, 120000))
            reject(QStringLiteral("Download timed out without progress."), true);
    });
    monitor.start(25);
    eventLoop.exec();
    consume();
    if (result.error.isEmpty() && reply->error() != QNetworkReply::NoError)
        result = {reply->errorString(), true};
    if (result.error.isEmpty() && (!accepted || written != expected))
        result = {QStringLiteral("The response ended before its declared length."), true};
    delete reply;
    if (!archive.flush() && result.error.isEmpty())
        result.error = archive.errorString();
    return result;
}
bool backoff(int attempt, const std::atomic_bool &cancelled)
{
    QElapsedTimer timer;
    timer.start();
    const int delay = 100 * (1 << std::min(attempt, 3));
    while (timer.elapsed() < delay && !cancelled.load())
        QThread::msleep(20);
    return !cancelled.load();
}
QString systemTar()
{
#ifdef Q_OS_WIN
    wchar_t path[MAX_PATH + 1]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    return length > 0 && length < MAX_PATH
        ? QDir(QString::fromWCharArray(path, int(length))).filePath(QStringLiteral("tar.exe")) : QString{};
#else
    return QStringLiteral("/usr/bin/tar");
#endif
}
QString extractArchive(const QString &program, const QString &zip, const QString &staging,
    const std::atomic_bool &cancelled)
{
    if (!QFileInfo(program).isAbsolute() || !QFileInfo(program).isFile())
        return QStringLiteral("The absolute system tar executable is unavailable.");
    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("-xf"), zip, QStringLiteral("-C"), staging});
    process.setWorkingDirectory(staging);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    QElapsedTimer timer;
    timer.start();
    QByteArray diagnostic;
    bool timedOut = false;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        const QByteArray block = process.readAll();
        if (diagnostic.size() < 65536)
            diagnostic += block.left(65536 - diagnostic.size());
        if (cancelled.load() || timer.elapsed() > 30 * 60 * 1000) {
            timedOut = !cancelled.load();
            process.kill();
            process.waitForFinished(5000);
            break;
        }
    }
    if (cancelled.load())
        return QStringLiteral("Extraction cancelled.");
    if (timedOut)
        return QStringLiteral("Extraction timed out.");
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0)
        return QStringLiteral("System tar failed: %1 %2").arg(process.errorString(), QString::fromLocal8Bit(diagnostic));
    return {};
}
bool removeOwnedStage(const QString &root, const QString &stage, const QString &id)
{
    const QString marker = stage + QStringLiteral(".owner.json");
    if (!withinRoot(root, stage) || !safeAncestors(stage) || !safeAncestors(marker)
        || !recordMatches(marker, id))
        return false;
    if (QFileInfo::exists(stage)) {
        QDirIterator iterator(stage, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (reparsePoint(iterator.next()))
                return false;
        }
        if (!QDir(stage).removeRecursively())
            return false;
    }
    return QFile::remove(marker);
}

struct StageGuard {
    QString root, path, id;
    ~StageGuard() { removeOwnedStage(root, path, id); }
};

QStringList cleanupOwnedCache(const QString &root, const QString &cache, const QString &id,
    const QStringList &dataNames)
{
    if (!QFileInfo::exists(cache) && !reparsePoint(cache))
        return {};
    const QString checkpoint = QDir(cache).filePath(QStringLiteral("checkpoint.json"));
    const auto preserved = [&] {
        return QStringList{QStringLiteral("Unknown or linked download cache was preserved: %1").arg(cache)};
    };
    if (!withinRoot(root, cache) || !safeAncestors(cache) || !QFileInfo(cache).isDir()
        || !recordMatches(checkpoint, id))
        return preserved();
    const auto entries = QDir(cache).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        if ((entry.fileName() != QStringLiteral("checkpoint.json") && !dataNames.contains(entry.fileName()))
            || !entry.isFile() || reparsePoint(entry.absoluteFilePath()))
            return preserved();
    }
    // Only verified, app-owned files are removed, and only after publication.
    for (const auto &name : dataNames) {
        const QString path = QDir(cache).filePath(name);
        if (QFileInfo::exists(path) && !QFile::remove(path))
            return {QStringLiteral("Verified download cache could not be removed: %1").arg(path)};
    }
    if (!QFile::remove(checkpoint) || !QDir().rmdir(cache))
        return {QStringLiteral("Download checkpoint could not be removed: %1").arg(cache)};
    return {};
}

bool prepareCache(const QString &cache, const QString &id, const QStringList &dataNames, QString *error)
{
    if (!safeAncestors(cache)) {
        *error = QStringLiteral("Download cache is linked.");
        return false;
    }
    const QString checkpoint = QDir(cache).filePath(QStringLiteral("checkpoint.json"));
    if (QFileInfo::exists(cache)) {
        if (!QFileInfo(cache).isDir() || !recordMatches(checkpoint, id)) {
            *error = QStringLiteral("Refusing an unknown download cache.");
            return false;
        }
        const auto entries = QDir(cache).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const auto &entry : entries) {
            if ((entry.fileName() != QStringLiteral("checkpoint.json") && !dataNames.contains(entry.fileName()))
                || !entry.isFile() || reparsePoint(entry.absoluteFilePath())) {
                *error = QStringLiteral("Download cache contains unknown or linked files.");
                return false;
            }
        }
        return true;
    }
    if (!QDir().mkdir(cache) || !writeRecord(checkpoint, id, error)) {
        if (error->isEmpty())
            *error = QStringLiteral("Could not create download cache.");
        return false;
    }
    return true;
}

bool enoughSpace(const QString &root, qint64 needed, qint64 expanded)
{
    constexpr qint64 reserve = 64 * 1024 * 1024;
    if (needed < 0 || expanded < 0 || expanded > std::numeric_limits<qint64>::max() - reserve
        || needed > std::numeric_limits<qint64>::max() - reserve - expanded)
        return false;
    const QStorageInfo storage(root);
    return !storage.isValid() || storage.bytesAvailable() < 0
        || storage.bytesAvailable() >= needed + expanded + reserve;
}

QString downloadFile(QNetworkAccessManager &manager, const QString &path,
    const FasterWhisperDownloadFile &file, const Request &request,
    const std::atomic_bool &cancelled, const Report &report,
    qint64 progressBase, qint64 progressTotal, const QString &component, qint64 expanded)
{
    if (!safeAncestors(path))
        return QStringLiteral("Cached file is linked.");
    QFile target(path);
    if (!target.open(QIODevice::ReadWrite))
        return target.errorString();
    if (target.size() > file.size)
        return QStringLiteral("Cached file exceeds its pinned size: %1").arg(file.name);
    QString error;
    const int attempts = std::clamp(request.maximumAttempts, 1, 5);
    for (int attempt = 0; attempt < attempts && !cancelled.load(); ++attempt) {
        if (target.size() == file.size) {
            report({State::Verifying, progressBase + file.size, progressTotal,
                QStringLiteral("Verifying %1").arg(file.name), component});
            if (fileHash(target, 0, file.size, cancelled, &error) == file.sha256)
                return {};
            if (cancelled.load())
                break;
            if (!target.resize(0))
                return target.errorString();
            error = QStringLiteral("SHA-256 mismatch: %1").arg(file.name);
        }
        if (!enoughSpace(request.installRoot, file.size - target.size(), expanded))
            return QStringLiteral("Insufficient disk space for download and preparation.");
        auto response = transfer(manager, target, file, 0, request, cancelled, report,
            progressBase, progressTotal, component);
        if (cancelled.load())
            break;
        if (target.size() == file.size && response.error.isEmpty()) {
            report({State::Verifying, progressBase + file.size, progressTotal,
                QStringLiteral("Verifying %1").arg(file.name), component});
            if (fileHash(target, 0, file.size, cancelled, &error) == file.sha256)
                return {};
            if (cancelled.load())
                break;
            if (!target.resize(0))
                return target.errorString();
            response = {QStringLiteral("SHA-256 mismatch: %1").arg(file.name), true};
        }
        // Only the current app-owned partial is restarted if a server rejects Range.
        if (response.error.startsWith(QStringLiteral("HTTP 416")) && !target.resize(0))
            return target.errorString();
        error = response.error.isEmpty() ? QStringLiteral("Incomplete download: %1").arg(file.name) : response.error;
        if (!response.retryable || attempt + 1 == attempts)
            break;
        if (!backoff(attempt, cancelled))
            break;
    }
    return cancelled.load() ? QStringLiteral("Preparation cancelled.") : error;
}

bool copyFile(const QString &source, const QString &destination, const std::atomic_bool &cancelled,
    QString *error)
{
    QFile input(source), output(destination);
    if (!safeAncestors(source) || !safeAncestors(destination)
        || !input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *error = QStringLiteral("Could not copy verified model file to private staging.");
        return false;
    }
    while (!input.atEnd() && !cancelled.load()) {
        const auto bytes = input.read(BufferSize);
        if ((bytes.isEmpty() && input.error() != QFileDevice::NoError)
            || output.write(bytes) != bytes.size()) {
            *error = QStringLiteral("Could not copy complete verified model data.");
            return false;
        }
    }
    if (cancelled.load()) {
        *error = QStringLiteral("Preparation cancelled.");
        return false;
    }
    if (!output.flush()) {
        *error = output.errorString();
        return false;
    }
    return true;
}
Result installPackage(const Request &request, const std::atomic_bool &cancelled, const Report &report)
{
    Result result;
    auto fail = [&](const QString &error) {
        result.state = cancelled.load() ? State::Cancelled : State::Failed;
        result.error = cancelled.load()
            ? QStringLiteral("Preparation cancelled; resumable downloads were retained.") : error;
        return result;
    };
    QString error;
    report({State::Preparing, 0, 0, QStringLiteral("Checking components"), {}});
    if (cancelled.load())
        return fail({});
    if (!validateFasterWhisperPackage(request.package, &error, request.allowLoopbackHttp))
        return fail(error);
    if (!request.existingEngineDirectory.isEmpty()) {
        if (!validateFasterWhisperEngine(request.existingEngineDirectory, &error))
            return fail(QStringLiteral("Existing engine is invalid: %1").arg(error));
        result.engineDirectory = QDir::cleanPath(request.existingEngineDirectory);
    }
    auto checkModel = [&](const QString &path) {
        return validateFasterWhisperModel(path, request.package.modelFiles, cancelled, &error,
            [&](qint64 done, qint64 total) {
                report({State::Verifying, done, total, QStringLiteral("Checking large-v2 model"), QStringLiteral("model")});
            });
    };
    if (!request.existingModelDirectory.isEmpty()) {
        if (!checkModel(request.existingModelDirectory))
            return fail(QStringLiteral("Existing model is invalid: %1").arg(error));
        result.modelDirectory = QDir::cleanPath(request.existingModelDirectory);
    }
    if (result.modelDirectory.isEmpty() && !result.engineDirectory.isEmpty()) {
        const auto bundled = QDir(result.engineDirectory).filePath(QStringLiteral("_models/faster-whisper-large-v2"));
        if (QFileInfo::exists(bundled) || reparsePoint(bundled)) {
            if (!checkModel(bundled))
                return fail(QStringLiteral("Existing bundled model will not be overwritten: %1").arg(error));
            result.modelDirectory = bundled;
        }
    }
    // User-selected components are read-only. No marker is added to those folders.
    if (!result.engineDirectory.isEmpty() && !result.modelDirectory.isEmpty()) {
        if (cancelled.load())
            return fail({});
        result.state = State::Ready;
        return result;
    }
    const QString root = QDir::cleanPath(QFileInfo(request.installRoot).absoluteFilePath());
    if (request.installRoot.isEmpty() || !QFileInfo(request.installRoot).isAbsolute()
        || QDir(root).isRoot() || !safeAncestors(root))
        return fail(QStringLiteral("Installation root must be absolute, non-root and without links."));
    if (QFileInfo::exists(root) && !QFileInfo(root).isDir())
        return fail(QStringLiteral("Installation root is not a directory."));
    QDir directory(root);
    if (directory.exists() && !directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()) {
        if (!recordMatches(directory.filePath(RootRecord), QStringLiteral("root")))
            return fail(QStringLiteral("Refusing a nonempty directory not owned by Arcanblade."));
    } else if (!QDir().mkpath(root) || !writeRecord(directory.filePath(RootRecord), QStringLiteral("root"), &error)) {
        return fail(QStringLiteral("Could not initialize installation root: %1").arg(error));
    }
    const QString lockPath = directory.filePath(QStringLiteral(".lock-xxl"));
    if (reparsePoint(lockPath))
        return fail(QStringLiteral("Installation lock is linked."));
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (QFileInfo::exists(lockPath)) {
        qint64 pid = 0;
        QString host, app;
        if (!lock.getLockInfo(&pid, &host, &app) || app != QCoreApplication::applicationName())
            return fail(QStringLiteral("Refusing an unknown installation lock."));
    }
    if (!lock.tryLock(0))
        return fail(QStringLiteral("Another preparation is using this destination."));

    QNetworkAccessManager manager;
    useAutomaticDownloadProxy(manager, cancelled);
    if (result.engineDirectory.isEmpty()) {
        const auto &file = request.package.engineArchive;
        const QString id = fingerprint(request.package.version, {file});
        const QString destination = directory.filePath(request.package.version);
        const QString cache = directory.filePath(QStringLiteral(".cache-engine-") + id);
        const QString archiveName = QStringLiteral("archive.7z.part");
        const QString archivePath = QDir(cache).filePath(archiveName);
        if (QFileInfo::exists(destination) || reparsePoint(destination)) {
            if (!safeAncestors(destination) || !recordMatches(QDir(destination).filePath(InstallRecord), id)
                || !validateFasterWhisperEngine(destination, &error))
                return fail(QStringLiteral("An existing engine will not be overwritten: %1").arg(error));
            result.engineDirectory = destination;
            result.warnings += cleanupOwnedCache(root, cache, id, {archiveName});
        } else {
            QString program = systemTar();
            if (!request.extractionProgramForTests.isEmpty()) {
                const bool allLoopback = loopback(file.url)
                    && std::all_of(request.package.modelFiles.cbegin(), request.package.modelFiles.cend(),
                        [](const auto &part) { return loopback(part.url); });
                if (!request.allowLoopbackHttp || !allLoopback)
                    return fail(QStringLiteral("Extraction override requires explicit loopback fixtures."));
                program = request.extractionProgramForTests;
            }
            if (!QFileInfo(program).isAbsolute() || !QFileInfo(program).isFile())
                return fail(QStringLiteral("The absolute system tar executable is unavailable."));
            if (!prepareCache(cache, id, {archiveName}, &error))
                return fail(error);
            error = downloadFile(manager, archivePath, file, request, cancelled, report,
                0, file.size, QStringLiteral("engine"), request.package.unpackedSize);
            if (cancelled.load() || !error.isEmpty())
                return fail(error);
            const auto inspection = inspectFasterWhisperArchive(archivePath, cancelled,
                request.package.unpackedSize > 0 ? request.package.unpackedSize : qint64(16) * 1024 * 1024 * 1024);
            if (!inspection.success())
                return fail(inspection.error);
            for (const auto &entry : inspection.entries) {
                if (QFileInfo(entry.path).fileName().compare(InstallRecord, Qt::CaseInsensitive) == 0)
                    return fail(QStringLiteral("The archive contains reserved installation metadata."));
            }
            if (!enoughSpace(root, 0, inspection.unpackedBytes))
                return fail(QStringLiteral("Insufficient disk space for extraction."));
            const QString stage = directory.filePath(QStringLiteral(".staging-engine-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces));
            if (!withinRoot(root, stage) || !QDir().mkdir(stage)
                || !writeRecord(stage + QStringLiteral(".owner.json"), id, &error))
                return fail(QStringLiteral("Could not create private extraction staging: %1").arg(error));
            StageGuard guard{root, stage, id};
            report({State::Extracting, 0, inspection.unpackedBytes,
                QStringLiteral("Extracting verified Faster-Whisper-XXL"), QStringLiteral("engine")});
            error = extractArchive(program, archivePath, stage, cancelled);
            if (error.isEmpty() && !validateFasterWhisperExtracted(stage, inspection, cancelled, &error)
                && error.isEmpty())
                error = QStringLiteral("Extracted files did not match the checked archive.");
            QString engine;
            if (error.isEmpty()) {
                engine = findFasterWhisperEngineRoot(stage, &error);
                if (!engine.isEmpty() && !validateFasterWhisperEngine(engine, &error))
                    engine.clear();
            }
            if (cancelled.load() || !error.isEmpty() || engine.isEmpty())
                return fail(error.isEmpty() ? QStringLiteral("No valid engine was extracted.") : error);
            if (!writeRecord(QDir(engine).filePath(InstallRecord), id, &error))
                return fail(error);
            if (!safeAncestors(destination) || QFileInfo::exists(destination) || !QDir().rename(engine, destination))
                return fail(QStringLiteral("Could not publish engine without overwriting existing files."));
            result.engineDirectory = destination;
            result.warnings += cleanupOwnedCache(root, cache, id, {archiveName});
        }
    }
    if (cancelled.load())
        return fail({});
    if (result.modelDirectory.isEmpty()) {
        const QString id = fingerprint(request.package.modelRevision, request.package.modelFiles);
        const QString legacyDestination = QDir(directory.filePath(QStringLiteral("models")))
            .filePath(QStringLiteral("faster-whisper-large-v2-") + request.package.modelRevision);
        const QString engineId = fingerprint(request.package.version, {request.package.engineArchive});
        const bool ownedEngine = withinRoot(root, result.engineDirectory)
            && recordMatches(QDir(result.engineDirectory).filePath(InstallRecord), engineId);
        const bool useLegacy = QFileInfo::exists(legacyDestination) || reparsePoint(legacyDestination) || !ownedEngine;
        const QString modelParent = useLegacy ? directory.filePath(QStringLiteral("models"))
            : QDir(result.engineDirectory).filePath(QStringLiteral("_models"));
        const QString destination = useLegacy ? legacyDestination
            : QDir(modelParent).filePath(QStringLiteral("faster-whisper-large-v2"));
        const QString cache = directory.filePath(QStringLiteral(".cache-model-") + id);
        QStringList cacheNames;
        qint64 total = 0;
        for (const auto &file : request.package.modelFiles) {
            cacheNames.append(file.name + QStringLiteral(".part"));
            total += file.size;
        }
        if (!safeAncestors(modelParent) || (QFileInfo::exists(modelParent) && !QFileInfo(modelParent).isDir()))
            return fail(QStringLiteral("Model parent is linked or is not a directory."));
        if (QFileInfo::exists(destination) || reparsePoint(destination)) {
            if (!safeAncestors(destination) || !recordMatches(QDir(destination).filePath(InstallRecord), id)
                || !checkModel(destination))
                return fail(QStringLiteral("An existing model will not be overwritten: %1").arg(error));
            result.modelDirectory = destination;
            result.warnings += cleanupOwnedCache(root, cache, id, cacheNames);
        } else {
            if (!QDir().mkpath(modelParent) || !prepareCache(cache, id, cacheNames, &error))
                return fail(error.isEmpty() ? QStringLiteral("Could not create model parent.") : error);
            qint64 completed = 0;
            for (const auto &file : request.package.modelFiles) {
                error = downloadFile(manager, QDir(cache).filePath(file.name + QStringLiteral(".part")),
                    file, request, cancelled, report, completed, total, QStringLiteral("model"), total);
                if (cancelled.load() || !error.isEmpty())
                    return fail(error);
                completed += file.size;
            }
            if (!enoughSpace(root, 0, total))
                return fail(QStringLiteral("Insufficient disk space to publish the verified model."));
            const QString stage = directory.filePath(QStringLiteral(".staging-model-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces));
            if (!withinRoot(root, stage) || !QDir().mkdir(stage)
                || !writeRecord(stage + QStringLiteral(".owner.json"), id, &error))
                return fail(QStringLiteral("Could not create model staging: %1").arg(error));
            StageGuard guard{root, stage, id};
            report({State::Verifying, 0, total, QStringLiteral("Preparing verified large-v2 model"), QStringLiteral("model")});
            for (const auto &file : request.package.modelFiles) {
                if (!copyFile(QDir(cache).filePath(file.name + QStringLiteral(".part")),
                        QDir(stage).filePath(file.name), cancelled, &error))
                    return fail(error);
            }
            if (!checkModel(stage) || cancelled.load())
                return fail(error);
            if (!writeRecord(QDir(stage).filePath(InstallRecord), id, &error))
                return fail(error);
            if (!safeAncestors(destination) || QFileInfo::exists(destination) || !QDir().rename(stage, destination))
                return fail(QStringLiteral("Could not publish model without overwriting existing files."));
            result.modelDirectory = destination;
            result.warnings += cleanupOwnedCache(root, cache, id, cacheNames);
        }
    }
    if (cancelled.load())
        return fail({});
    result.state = State::Ready;
    report({State::Ready, 1, 1, QStringLiteral("Faster-Whisper-XXL and large-v2 are ready"), {}});
    return result;
}
} // namespace
struct FasterWhisperInstallService::WorkerState {
    std::atomic_bool cancelled{false};
    QMutex mutex;
    Progress latest;
    bool changed = false;
};
FasterWhisperInstallService::FasterWhisperInstallService(QObject *parent, Executor executor)
    : QObject(parent), m_executor(executor ? std::move(executor) : Executor(installPackage)),
      m_progressTimer(new QTimer(this))
{
    qRegisterMetaType<Progress>();
    qRegisterMetaType<Result>();
    m_progressTimer->setInterval(75);
    connect(m_progressTimer, &QTimer::timeout, this, &FasterWhisperInstallService::flushProgress);
}
FasterWhisperInstallService::~FasterWhisperInstallService()
{
    cancel(); // Worker owns shared state and promise, never captures this QObject.
}
bool FasterWhisperInstallService::start(const Request &request)
{
    if (m_busy || m_shuttingDown)
        return false;
    m_busy = true;
    m_result = {};
    m_progress = {State::Preparing, 0, 0, QStringLiteral("Preparing installation")};
    m_worker = std::make_shared<WorkerState>();
    const auto worker = m_worker;
    const auto executor = m_executor;
    auto promise = std::make_shared<QPromise<Result>>();
    auto *watcher = new QFutureWatcher<Result>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher] {
        const QPointer<FasterWhisperInstallService> guard(this);
        flushProgress();
        if (!guard)
            return;
        const Result completedResult = watcher->result();
        m_result = completedResult;
        m_progress.state = completedResult.state;
        m_progress.message = completedResult.error;
        if (completedResult.state == State::Ready)
            m_progress.completedBytes = m_progress.totalBytes;
        const Progress completedProgress = m_progress;
        m_busy = false;
        m_progressTimer->stop();
        m_worker.reset();
        m_watcher = nullptr;
        watcher->deleteLater();
        // Direct signal handlers may start another task, request shutdown or destroy the service.
        emit progressChanged(completedProgress);
        if (!guard)
            return;
        emit finished(completedResult);
        if (!guard)
            return;
        if (m_shuttingDown && !m_busy && !m_shutdownEmitted) {
            m_shutdownEmitted = true;
            emit shutdownFinished();
        }
    });
    watcher->setFuture(promise->future());
    m_progressTimer->start();
    QThreadPool::globalInstance()->start([promise, executor, worker, request] {
        promise->start();
        Result result;
        try {
            result = executor(request, worker->cancelled, [worker](const Progress &progress) {
                QMutexLocker lock(&worker->mutex);
                worker->latest = progress;
                worker->changed = true;
            });
        } catch (const std::exception &exception) {
            result = failure(QString::fromUtf8(exception.what()), worker->cancelled);
        } catch (...) {
            result = failure(QStringLiteral("Unexpected installation error."), worker->cancelled);
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
    // Initialize watcher and cancellation before calling user code through this signal.
    emit progressChanged(m_progress);
    return true;
}

void FasterWhisperInstallService::cancel()
{
    if (m_worker)
        m_worker->cancelled.store(true);
}
void FasterWhisperInstallService::shutdown()
{
    m_shuttingDown = true;
    cancel();
    if (!m_busy && !m_shutdownEmitted) {
        m_shutdownEmitted = true;
        emit shutdownFinished();
    }
}
bool FasterWhisperInstallService::isBusy() const { return m_busy; }
FasterWhisperInstallService::Progress FasterWhisperInstallService::progress() const { return m_progress; }
FasterWhisperInstallService::Result FasterWhisperInstallService::result() const { return m_result; }
void FasterWhisperInstallService::flushProgress()
{
    if (!m_worker)
        return;
    bool changed = false;
    {
        QMutexLocker lock(&m_worker->mutex);
        changed = m_worker->changed;
        if (changed) {
            m_progress = m_worker->latest;
            m_worker->changed = false;
        }
    }
    if (changed)
        emit progressChanged(m_progress);
}
} // namespace asmr
