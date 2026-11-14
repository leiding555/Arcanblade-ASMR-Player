#include "ai/ChickenRiceInstallService.h"
#include "network/DownloadProxyResolver.h"
#include "ai/ChickenRiceArchive.h"
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
using Request = ChickenRiceInstallRequest;
using Result = ChickenRiceInstallResult;
using Progress = ChickenRiceInstallProgress;
using State = ChickenRiceInstallState;
using Report = std::function<void(const Progress &)>;
const QString Owner = QStringLiteral("Arcanblade.ChickenRice.Install.v1");
const QString RootRecord = QStringLiteral(".arcanblade-install-root.json");
const QString InstallRecord = QStringLiteral(".arcanblade-install.json");
const QString StageRecord = QStringLiteral(".arcanblade-stage.json");
constexpr qint64 BufferSize = 1024 * 1024;

Result failure(const QString &error, const std::atomic_bool &cancelled)
{
    return {cancelled.load() ? State::Cancelled : State::Failed, {},
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
QString fingerprint(const ChickenRicePackage &package)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(package.version.toUtf8());
    hash.addData(package.archiveName.toUtf8());
    hash.addData(package.sha256);
    hash.addData(QByteArray::number(package.size));
    for (const auto &part : package.parts) {
        hash.addData(part.name.toUtf8());
        hash.addData(QByteArray::number(part.size));
        hash.addData(part.sha256);
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
    const ChickenRicePackagePart &part, qint64 base, const Request &request,
    const std::atomic_bool &cancelled, const Report &report)
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
            report({State::Downloading, archive.pos(), request.package.size,
                QStringLiteral("Downloading %1").arg(part.name)});
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
quint32 crc32Update(quint32 crc, const QByteArray &bytes)
{
    static const auto table = [] {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
            values[i] = value;
        }
        return values;
    }();
    for (const char value : bytes)
        crc = table[(crc ^ quint8(value)) & 0xff] ^ (crc >> 8);
    return crc;
}
QString validateExtracted(const QString &staging, const ChickenRiceArchiveInspection &inspection,
    const std::atomic_bool &cancelled)
{
    QSet<QString> expected;
    for (const auto &entry : inspection.entries) {
        if (cancelled.load())
            return QStringLiteral("Validation cancelled.");
        const QString path = QDir(staging).filePath(entry.path);
        if (!withinRoot(staging, path) || !safeAncestors(path))
            return QStringLiteral("Extracted path escaped staging or is linked: %1").arg(entry.path);
        const QFileInfo info(path);
        if (!info.exists() || (entry.directory ? !info.isDir() : !info.isFile())
            || (!entry.directory && info.size() != entry.size))
            return QStringLiteral("Extracted entry type or size mismatch: %1").arg(entry.path);
        QString relative = QDir::cleanPath(entry.path);
        expected.insert(relative.toCaseFolded());
        while (relative.contains(QLatin1Char('/'))) {
            relative = relative.left(relative.lastIndexOf(QLatin1Char('/')));
            expected.insert(relative.toCaseFolded());
        }
        if (!entry.directory) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return file.errorString();
            quint32 crc = 0xffffffffU;
            while (!file.atEnd() && !cancelled.load()) {
                const QByteArray block = file.read(BufferSize);
                if (block.isEmpty() && file.error() != QFileDevice::NoError)
                    return file.errorString();
                crc = crc32Update(crc, block);
            }
            if (cancelled.load())
                return QStringLiteral("Validation cancelled.");
            if ((crc ^ 0xffffffffU) != entry.crc32)
                return QStringLiteral("Extracted entry CRC mismatch: %1").arg(entry.path);
        }
    }
    expected.insert(StageRecord.toCaseFolded());
    QDirIterator iterator(staging, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (cancelled.load())
            return QStringLiteral("Validation cancelled.");
        if (reparsePoint(path) || !expected.contains(QDir(staging).relativeFilePath(path).toCaseFolded()))
            return QStringLiteral("Unexpected or linked extracted entry: %1").arg(path);
    }
    return {};
}
bool removeOwnedStage(const QString &root, const QString &stage, const QString &id)
{
    // Verify the resolved recursive deletion target stays inside the owned root.
    if (!withinRoot(root, stage) || !safeAncestors(stage)
        || !recordMatches(QDir(stage).filePath(StageRecord), id))
        return false;
    QDirIterator iterator(stage, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        if (reparsePoint(iterator.next()))
            return false;
    }
    return QDir(stage).removeRecursively();
}
QStringList cleanupOwnedCache(const QString &root, const QString &cache, const QString &id)
{
    if (!QFileInfo::exists(cache) && !reparsePoint(cache))
        return {};
    const QString checkpoint = QDir(cache).filePath(QStringLiteral("checkpoint.json"));
    const QString archive = QDir(cache).filePath(QStringLiteral("archive.zip.part"));
    const auto preserved = [&] {
        return QStringList{QStringLiteral("Unknown or linked download cache was preserved: %1").arg(cache)};
    };
    if (!withinRoot(root, cache) || !safeAncestors(cache) || !QFileInfo(cache).isDir()
        || !recordMatches(checkpoint, id))
        return preserved();
    const auto entries = QDir(cache).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        if ((entry.fileName() != QStringLiteral("checkpoint.json")
                && entry.fileName() != QStringLiteral("archive.zip.part"))
            || !entry.isFile() || reparsePoint(entry.absoluteFilePath()))
            return preserved();
    }
    // Remove only the two validated app-owned files; never recursively erase a cache.
    // Keep its ownership checkpoint when a sharing violation prevents ZIP removal.
    if (QFileInfo::exists(archive) && !QFile::remove(archive))
        return {QStringLiteral("Verified ZIP cache could not be removed: %1").arg(archive)};
    if (!QFile::remove(checkpoint))
        return {QStringLiteral("Download checkpoint could not be removed: %1").arg(checkpoint)};
    if (!QDir().rmdir(cache))
        return {QStringLiteral("Download cache directory could not be removed: %1").arg(cache)};
    return {};
}

Result installPackage(const Request &request, const std::atomic_bool &cancelled, const Report &report)
{
    report({State::Preparing, 0, request.package.size, QStringLiteral("Checking destination")});
    QString error;
    if (cancelled.load())
        return failure({}, cancelled);
    if (!validateChickenRicePackage(request.package, &error, request.allowLoopbackHttp))
        return failure(error, cancelled);
    const QString root = QDir::cleanPath(QFileInfo(request.installRoot).absoluteFilePath());
    if (request.installRoot.isEmpty() || !QFileInfo(request.installRoot).isAbsolute()
        || QDir(root).isRoot() || !safeAncestors(root))
        return failure(QStringLiteral("Installation root must be absolute, non-root and without links."), cancelled);
    if (QFileInfo::exists(root) && !QFileInfo(root).isDir())
        return failure(QStringLiteral("Installation root is not a directory."), cancelled);
    QDir directory(root);
    if (directory.exists() && !directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()) {
        if (!recordMatches(directory.filePath(RootRecord), QStringLiteral("root")))
            return failure(QStringLiteral("Refusing a nonempty directory not owned by Arcanblade."), cancelled);
    } else if (!QDir().mkpath(root)
        || !writeRecord(directory.filePath(RootRecord), QStringLiteral("root"), &error)) {
        return failure(QStringLiteral("Could not initialize installation root: %1").arg(error), cancelled);
    }
    const QString key = request.package.version + QLatin1Char('-') + chickenRiceVariantKey(request.package.variant);
    static const QRegularExpression safeKey(QStringLiteral("^[A-Za-z0-9._-]+$"));
    if (!safeKey.match(key).hasMatch())
        return failure(QStringLiteral("Invalid package destination key."), cancelled);
    const QString id = fingerprint(request.package);
    const QString destination = directory.filePath(key);
    const QString lockPath = directory.filePath(QStringLiteral(".lock-") + key);
    if (reparsePoint(lockPath))
        return failure(QStringLiteral("Installation lock is a link."), cancelled);
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (QFileInfo::exists(lockPath)) {
        qint64 pid = 0;
        QString host, app;
        if (!lock.getLockInfo(&pid, &host, &app) || app != QCoreApplication::applicationName())
            return failure(QStringLiteral("Refusing an unknown installation lock."), cancelled);
    }
    if (!lock.tryLock(0))
        return failure(QStringLiteral("Another installation is using this destination."), cancelled);
    const QString cache = directory.filePath(QStringLiteral(".cache-") + key);
    const QString checkpoint = QDir(cache).filePath(QStringLiteral("checkpoint.json"));
    const QString archivePath = QDir(cache).filePath(QStringLiteral("archive.zip.part"));
    if (QFileInfo::exists(destination)) {
        if (!safeAncestors(destination) || !recordMatches(QDir(destination).filePath(InstallRecord), id)
            || !validateChickenRiceEngine(destination, &error))
            return failure(QStringLiteral("An existing installation will not be overwritten: %1").arg(error), cancelled);
        if (cancelled.load())
            return failure({}, cancelled);
        return {State::Ready, destination, {}, cleanupOwnedCache(root, cache, id)};
    }
    // Check the extractor before downloading gigabytes; an existing valid engine needs no tar.
    QString program = systemTar();
    if (!request.extractionProgramForTests.isEmpty()) {
        const bool allLoopback = std::all_of(request.package.parts.cbegin(), request.package.parts.cend(),
            [](const auto &part) { return loopback(part.url); });
        if (!request.allowLoopbackHttp || !allLoopback)
            return failure(QStringLiteral("Extraction test override requires explicit loopback fixtures."), cancelled);
        program = request.extractionProgramForTests;
    }
    if (!QFileInfo(program).isAbsolute() || !QFileInfo(program).isFile())
        return failure(QStringLiteral("The absolute system tar executable is unavailable."), cancelled);
    if (!safeAncestors(cache))
        return failure(QStringLiteral("Download cache is linked."), cancelled);
    if (QFileInfo::exists(cache)) {
        if (!QFileInfo(cache).isDir() || !recordMatches(checkpoint, id))
            return failure(QStringLiteral("Refusing an unknown download cache."), cancelled);
        const auto files = QDir(cache).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QString &name : files) {
            if ((name != QStringLiteral("checkpoint.json") && name != QStringLiteral("archive.zip.part"))
                || reparsePoint(QDir(cache).filePath(name)))
                return failure(QStringLiteral("Download cache contains unknown or linked files."), cancelled);
        }
    } else if (!QDir().mkdir(cache) || !writeRecord(checkpoint, id, &error)) {
        return failure(QStringLiteral("Could not initialize download cache: %1").arg(error), cancelled);
    }
    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadWrite))
        return failure(archive.errorString(), cancelled);
    if (archive.size() > request.package.size)
        return failure(QStringLiteral("Cached ZIP exceeds the known package size."), cancelled);
    QNetworkAccessManager manager;
    useAutomaticDownloadProxy(manager, cancelled);
    qint64 base = 0;
    const int attempts = std::clamp(request.maximumAttempts, 1, 5);
    for (const auto &part : request.package.parts) {
        if (cancelled.load())
            return failure({}, cancelled);
        bool complete = false;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (archive.size() >= base + part.size) {
                report({State::Verifying, base + part.size, request.package.size,
                    QStringLiteral("Verifying %1").arg(part.name)});
                if (fileHash(archive, base, part.size, cancelled, &error) == part.sha256) {
                    complete = true;
                    break;
                }
                if (cancelled.load())
                    return failure({}, cancelled);
                if (!archive.resize(base))
                    return failure(archive.errorString(), cancelled);
                error = QStringLiteral("Fragment SHA-256 mismatch: %1").arg(part.name);
            }
            const QStorageInfo storage(root);
            const qint64 needed = request.package.size - archive.size();
            const qint64 unpacked = request.package.unpackedSize;
            constexpr qint64 reserve = 64 * 1024 * 1024;
            if (unpacked < 0 || unpacked > std::numeric_limits<qint64>::max() - reserve
                || needed > std::numeric_limits<qint64>::max() - reserve - unpacked
                || (storage.isValid() && storage.bytesAvailable() >= 0
                    && storage.bytesAvailable() < needed + unpacked + reserve))
                return failure(QStringLiteral("Insufficient disk space for download and extraction."), cancelled);
            auto response = transfer(manager, archive, part, base, request, cancelled, report);
            if (cancelled.load())
                return failure({}, cancelled);
            if (archive.size() >= base + part.size && response.error.isEmpty()) {
                report({State::Verifying, base + part.size, request.package.size,
                    QStringLiteral("Verifying %1").arg(part.name)});
                if (fileHash(archive, base, part.size, cancelled, &error) == part.sha256) {
                    complete = true;
                    break;
                }
                if (!archive.resize(base))
                    return failure(archive.errorString(), cancelled);
                response = {QStringLiteral("Fragment SHA-256 mismatch: %1").arg(part.name), true};
            }
            if (response.error.isEmpty() && archive.size() < base + part.size)
                response = {QStringLiteral("Fragment response was partial: %1").arg(part.name), true};
            error = response.error.isEmpty() ? QStringLiteral("Fragment incomplete: %1").arg(part.name) : response.error;
            if (!response.retryable || attempt + 1 == attempts)
                break;
            if (!backoff(attempt, cancelled))
                return failure({}, cancelled);
        }
        if (!complete)
            return failure(error, cancelled);
        base += part.size;
    }
    report({State::Verifying, request.package.size, request.package.size, QStringLiteral("Verifying complete ZIP")});
    if (archive.size() != request.package.size
        || fileHash(archive, 0, request.package.size, cancelled, &error) != request.package.sha256)
        return failure(QStringLiteral("Complete ZIP SHA-256 mismatch. %1").arg(error), cancelled);
    archive.close();
    const auto inspection = inspectChickenRiceArchive(archivePath, cancelled);
    if (!inspection.success())
        return failure(inspection.error, cancelled);
    for (const auto &entry : inspection.entries) {
        const QString leaf = QFileInfo(entry.path).fileName();
        if (leaf.compare(StageRecord, Qt::CaseInsensitive) == 0
            || leaf.compare(InstallRecord, Qt::CaseInsensitive) == 0)
            return failure(QStringLiteral("The ZIP contains reserved installation metadata."), cancelled);
    }
    const QStorageInfo storage(root);
    constexpr qint64 reserve = 64 * 1024 * 1024;
    if (inspection.unpackedBytes > std::numeric_limits<qint64>::max() - reserve
        || (storage.isValid() && storage.bytesAvailable() >= 0
            && storage.bytesAvailable() < inspection.unpackedBytes + reserve))
        return failure(QStringLiteral("Insufficient disk space to extract the package."), cancelled);
    const QString stage = directory.filePath(QStringLiteral(".staging-") + key + QLatin1Char('-')
        + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!withinRoot(root, stage) || !QDir().mkdir(stage)
        || !writeRecord(QDir(stage).filePath(StageRecord), id, &error))
        return failure(QStringLiteral("Could not create private staging: %1").arg(error), cancelled);
    report({State::Extracting, 0, inspection.unpackedBytes, QStringLiteral("Extracting verified package")});
    error = extractArchive(program, archivePath, stage, cancelled);
    if (error.isEmpty()) {
        report({State::Verifying, 0, inspection.unpackedBytes, QStringLiteral("Checking extracted files")});
        error = validateExtracted(stage, inspection, cancelled);
    }
    QString engine;
    if (error.isEmpty()) {
        engine = findChickenRiceEngineRoot(stage, &error);
        if (!engine.isEmpty() && !validateChickenRiceEngine(engine, &error))
            engine.clear();
    }
    if (cancelled.load() || !error.isEmpty() || engine.isEmpty()) {
        const bool cleaned = removeOwnedStage(root, stage, id);
        auto result = failure(error.isEmpty() ? QStringLiteral("No valid engine was extracted.") : error, cancelled);
        if (!cleaned)
            result.warnings.append(QStringLiteral("Private staging could not be removed: %1").arg(stage));
        return result;
    }
    if (!writeRecord(QDir(engine).filePath(InstallRecord), id, &error)) {
        removeOwnedStage(root, stage, id);
        return failure(error, cancelled);
    }
    // Same-volume rename publishes to an absent destination; never replace an installation.
    if (QFileInfo::exists(destination) || !QDir().rename(engine, destination)) {
        removeOwnedStage(root, stage, id);
        return failure(QStringLiteral("Could not atomically publish installation; existing files were preserved."), cancelled);
    }
    Result result{State::Ready, destination, {}, {}};
    if (engine == stage) {
        if (!QFile::remove(QDir(destination).filePath(StageRecord)))
            result.warnings.append(QStringLiteral("Installation staging marker could not be removed."));
    } else if (!removeOwnedStage(root, stage, id)) {
        result.warnings.append(QStringLiteral("Empty staging could not be removed: %1").arg(stage));
    }
    result.warnings.append(cleanupOwnedCache(root, cache, id));
    return result;
}
} // namespace

struct ChickenRiceInstallService::WorkerState {
    std::atomic_bool cancelled{false};
    QMutex mutex;
    Progress latest;
    bool changed = false;
};
ChickenRiceInstallService::ChickenRiceInstallService(QObject *parent, Executor executor)
    : QObject(parent), m_executor(executor ? std::move(executor) : Executor(installPackage)),
      m_progressTimer(new QTimer(this))
{
    qRegisterMetaType<Progress>();
    qRegisterMetaType<Result>();
    m_progressTimer->setInterval(75);
    connect(m_progressTimer, &QTimer::timeout, this, &ChickenRiceInstallService::flushProgress);
}
ChickenRiceInstallService::~ChickenRiceInstallService()
{
    cancel(); // Worker owns shared state and promise, never captures this QObject.
}
bool ChickenRiceInstallService::start(const Request &request)
{
    if (m_busy || m_shuttingDown)
        return false;
    m_busy = true;
    m_result = {};
    m_progress = {State::Preparing, 0, request.package.size, QStringLiteral("Preparing installation")};
    m_worker = std::make_shared<WorkerState>();
    const auto worker = m_worker;
    const auto executor = m_executor;
    auto promise = std::make_shared<QPromise<Result>>();
    auto *watcher = new QFutureWatcher<Result>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher] {
        const QPointer<ChickenRiceInstallService> guard(this);
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

void ChickenRiceInstallService::cancel()
{
    if (m_worker)
        m_worker->cancelled.store(true);
}
void ChickenRiceInstallService::shutdown()
{
    m_shuttingDown = true;
    cancel();
    if (!m_busy && !m_shutdownEmitted) {
        m_shutdownEmitted = true;
        emit shutdownFinished();
    }
}
bool ChickenRiceInstallService::isBusy() const { return m_busy; }
ChickenRiceInstallService::Progress ChickenRiceInstallService::progress() const { return m_progress; }
ChickenRiceInstallService::Result ChickenRiceInstallService::result() const { return m_result; }
void ChickenRiceInstallService::flushProgress()
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
