#include "update/UpdatePackage.h"

#include "ai/ChickenRiceArchive.h"
#include "update/SemanticVersion.h"
#include "update/UpdateInstaller.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <filesystem>
#include <map>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
namespace fs = std::filesystem;
constexpr qint64 MaximumTreeBytes = 4LL * 1024 * 1024 * 1024;
constexpr qint64 MaximumFileBytes = 1024LL * 1024 * 1024;
constexpr qsizetype MaximumFiles = 10000;
constexpr qint64 MaximumJsonBytes = 16 * 1024 * 1024;
fs::path nativePath(const QString &path) { return fs::path(QDir::toNativeSeparators(path).toStdWString()).lexically_normal(); }
QString textPath(const fs::path &path) { return QDir::fromNativeSeparators(QString::fromStdWString(path.wstring())); }
QString textError(const std::wstring &message) { return QString::fromStdWString(message); }
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
QString pathKey(const QString &path) { return path.normalized(QString::NormalizationForm_C).toCaseFolded(); }
bool safe(const QString &path) { return QDir::isAbsolutePath(path) && updater::safeAncestors(nativePath(path)); }
bool safeRelative(const QString &path) {
    return updater::safeRelativePath(path.toUtf8().toStdString()) && path.count(QLatin1Char('/')) < 32;
}
QByteArray hash(const QString &path, const std::atomic_bool &cancelled, QString *error)
{
    QFile file(path);
    if (!safe(path) || !QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("无法读取安全的更新文件：%1").arg(path)); return {};
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled.load()) { fail(error, QStringLiteral("更新准备已取消。")); return {}; }
        const auto bytes = file.read(1024 * 1024);
        if (bytes.isEmpty() && file.error() != QFile::NoError) { fail(error, QStringLiteral("读取更新文件失败。")); return {}; }
        digest.addData(bytes);
    }
    return digest.result();
}
bool readJson(const QString &path, QJsonObject &object, QString *error)
{
    QFile file(path);
    if (!safe(path) || !QFileInfo(path).isFile() || file.size() <= 0 || file.size() > MaximumJsonBytes || !file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("更新元数据缺失、过大或不可读取：%1").arg(QFileInfo(path).fileName()));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return fail(error, QStringLiteral("更新元数据格式错误。"));
    object = document.object(); return true;
}
bool x64Executable(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto header = file.read(64);
    if (header.size() != 64 || header.left(2) != "MZ") return false;
    const auto offset = qFromLittleEndian<quint32>(header.constData() + 60);
    if (offset > 16 * 1024 * 1024 || !file.seek(offset)) return false;
    const auto pe = file.read(6);
    return pe.size() == 6 && pe.left(4) == QByteArray("PE\0\0", 4) && qFromLittleEndian<quint16>(pe.constData() + 4) == 0x8664;
}
bool loadManifest(const QString &root, const QString &version, const std::atomic_bool &cancelled,
                  std::vector<updater::File> &files, QString *error, const UpdatePackageProgress &progress,
                  bool exactTree)
{
    if (!SemanticVersion::parse(version) || !safe(root) || !QFileInfo(root).isDir())
        return fail(error, QStringLiteral("更新包目录或版本无效。"));
    QJsonObject manifest, metadata;
    if (!readJson(QDir(root).filePath(QStringLiteral("FILES.json")), manifest, error)
        || !readJson(QDir(root).filePath(QStringLiteral("BUILD-INFO.json")), metadata, error)) return false;
    if (manifest.value(QStringLiteral("SchemaVersion")).toInt() != 1 || !manifest.value(QStringLiteral("Files")).isArray())
        return fail(error, QStringLiteral("不支持的更新文件清单格式。"));
    if (metadata.value(QStringLiteral("Version")).toString() != version
        || metadata.value(QStringLiteral("ExecutableVersion")).toString() != version
        || metadata.value(QStringLiteral("Architecture")).toString() != QStringLiteral("x64")
        || metadata.value(QStringLiteral("Configuration")).toString() != QStringLiteral("Release")
        || metadata.value(QStringLiteral("UpdateProtocol")).toInt() != 1)
        return fail(error, QStringLiteral("此包的版本、架构或更新协议不匹配，请使用 GitHub 发布页手动下载。"));
    const auto entries = manifest.value(QStringLiteral("Files")).toArray();
    if (entries.isEmpty() || entries.size() > MaximumFiles) return fail(error, QStringLiteral("更新清单为空或文件数过多。"));
    static const QRegularExpression digestPattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    QSet<QString> seen, requiredDirectories;
    qint64 total = 0;
    files.clear(); files.reserve(std::size_t(entries.size()) + 1);
    for (const auto &value : entries) {
        if (cancelled.load()) return fail(error, QStringLiteral("更新准备已取消。"));
        if (!value.isObject()) return fail(error, QStringLiteral("更新文件记录无效。"));
        const auto entry = value.toObject();
        const auto path = entry.value(QStringLiteral("Path")).toString();
        const auto length = entry.value(QStringLiteral("Length"));
        const qint64 size = length.toInteger(-1);
        const auto digest = entry.value(QStringLiteral("Sha256")).toString();
        const auto key = pathKey(path);
        if (!safeRelative(path) || !updater::managedFilePath(path.toUtf8().toStdString()) || key == QStringLiteral("files.json") || seen.contains(key)
            || !length.isDouble() || size < 0 || size > MaximumFileBytes || length.toDouble() != double(size)
            || total > MaximumTreeBytes - size || !digestPattern.match(digest).hasMatch())
            return fail(error, QStringLiteral("更新清单存在不安全、重复或过大的文件。"));
        seen.insert(key); total += size;
        QString parent = key;
        while (parent.contains(QLatin1Char('/'))) {
            parent = parent.left(parent.lastIndexOf(QLatin1Char('/'))); requiredDirectories.insert(parent);
        }
        updater::File record;
        record.path = path.toUtf8().toStdString(); record.length = std::uint64_t(size);
        const auto rawDigest = QByteArray::fromHex(digest.toLatin1());
        std::copy(rawDigest.begin(), rawDigest.end(), record.sha256.begin());
        files.push_back(std::move(record));
    }
    for (const auto &key : seen) if (requiredDirectories.contains(key)) return fail(error, QStringLiteral("更新清单中的文件被用作目录。"));
    const QStringList required{
        QStringLiteral("Arcanblade.exe"), QStringLiteral("ArcanbladeUpdater.exe"), QStringLiteral("BUILD-INFO.json"),
        QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Gui.dll"), QStringLiteral("Qt6Widgets.dll"),
        QStringLiteral("Qt6Multimedia.dll"), QStringLiteral("Qt6Sql.dll"), QStringLiteral("Qt6Network.dll"), QStringLiteral("Qt6Pdf.dll"),
        QStringLiteral("platforms/qwindows.dll"), QStringLiteral("sqldrivers/qsqlite.dll"),
        QStringLiteral("multimedia/ffmpegmediaplugin.dll"), QStringLiteral("tls/qschannelbackend.dll"),
        QStringLiteral("msvcp140.dll"), QStringLiteral("vcruntime140.dll"), QStringLiteral("vcruntime140_1.dll")};
    for (const auto &path : required) if (!seen.contains(pathKey(path))) return fail(error, QStringLiteral("更新包缺少运行文件：%1").arg(path));
    for (const auto &prefix : {"avcodec-", "avformat-", "avutil-", "swresample-", "swscale-"}) {
        bool found = false;
        for (const auto &record : files) if (record.path.rfind(prefix, 0) == 0 && record.path.ends_with(".dll") && record.path.find('/') == std::string::npos) found = true;
        if (!found) return fail(error, QStringLiteral("更新包缺少 FFmpeg 运行库。"));
    }
    qint64 completed = 0;
    for (const auto &record : files) {
        const auto path = QDir(root).filePath(QString::fromUtf8(record.path));
        const QFileInfo info(path);
        if (!safe(path) || !info.isFile() || info.size() != qint64(record.length)) return fail(error, QStringLiteral("更新文件缺失或大小不符：%1").arg(info.fileName()));
        const auto actual = hash(path, cancelled, error);
        if (actual.size() != 32 || !std::equal(actual.begin(), actual.end(), record.sha256.begin(), [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; }))
            return fail(error, cancelled.load() ? QStringLiteral("更新准备已取消。") : QStringLiteral("更新文件校验失败：%1").arg(info.fileName()));
        if (record.path == "Arcanblade.exe" && QString::fromLatin1(actual.toHex()) != metadata.value(QStringLiteral("ExecutableSha256")).toString().toLower())
            return fail(error, QStringLiteral("更新主程序摘要与版本信息不匹配。"));
        if ((record.path == "Arcanblade.exe" || record.path == "ArcanbladeUpdater.exe") && !x64Executable(path))
            return fail(error, QStringLiteral("更新包中的程序不是有效的 Windows x64 可执行文件。"));
        completed += qint64(record.length); if (progress) progress(completed, total);
    }
    const QString manifestPath = QDir(root).filePath(QStringLiteral("FILES.json"));
    const auto manifestDigest = hash(manifestPath, cancelled, error);
    if (manifestDigest.size() != 32) return false;
    updater::File self;
    self.path = "FILES.json"; self.length = std::uint64_t(QFileInfo(manifestPath).size());
    std::copy(manifestDigest.begin(), manifestDigest.end(), self.sha256.begin()); files.push_back(self);
    if (exactTree) {
        seen.insert(QStringLiteral("files.json"));
        QSet<QString> found;
        QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (cancelled.load()) return fail(error, QStringLiteral("更新准备已取消。"));
            const auto path = iterator.next();
            const auto relative = QDir(root).relativeFilePath(path);
            const auto info = iterator.fileInfo();
            if (!safe(path) || (info.isDir() ? !requiredDirectories.contains(pathKey(relative))
                                           : (!info.isFile() || !seen.contains(pathKey(relative)))))
                return fail(error, QStringLiteral("更新包存在清单外的文件、目录或链接。"));
            if (info.isFile()) found.insert(pathKey(relative));
        }
        if (found != seen) return fail(error, QStringLiteral("更新文件清单与实际目录不一致。"));
    }
    return true;
}
QString systemTar()
{
#ifdef Q_OS_WIN
    wchar_t buffer[MAX_PATH + 1]{};
    const UINT count = GetSystemDirectoryW(buffer, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) return {};
    return QDir(QString::fromWCharArray(buffer, int(count))).filePath(QStringLiteral("tar.exe"));
#else
    return QStandardPaths::findExecutable(QStringLiteral("bsdtar"));
#endif
}
bool marker(const QString &directory, const QString &name, const QByteArray &content)
{
    const auto path = QDir(directory).filePath(name);
    if (!safe(path)) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.commit();
}
bool matchingMarker(const QString &directory, const QString &name, const QByteArray &content)
{
    const auto path = QDir(directory).filePath(name);
    QFile file(path);
    return safe(path) && file.size() <= 1024 && file.open(QIODevice::ReadOnly) && file.readAll() == content;
}
void releaseInstallLease(const updater::Plan &plan)
{
    const auto directory = QFileInfo(textPath(plan.packageDirectory)).absolutePath();
    if (matchingMarker(directory, QStringLiteral(".install-lease"), textPath(plan.sessionDirectory).toUtf8()))
        (void)QFile::remove(QDir(directory).filePath(QStringLiteral(".install-lease")));
}
bool loadPreparation(const UpdateInstallPreparation &preparation, updater::Plan &plan, QString *error)
{
    std::wstring nativeError;
    if (!preparation.success() || !safe(preparation.sessionDirectory) || !safe(preparation.planPath)
        || !updater::readPlan(nativePath(preparation.planPath), plan, &nativeError)
        || textPath(plan.sessionDirectory).compare(QDir::cleanPath(preparation.sessionDirectory), Qt::CaseInsensitive) != 0
        || preparation.planPath != QDir(preparation.sessionDirectory).filePath(QStringLiteral("install.plan"))
        || preparation.helperPath != QDir(preparation.sessionDirectory).filePath(QStringLiteral("ArcanbladeUpdater.exe")))
        return fail(error, QStringLiteral("更新准备信息无效：%1").arg(textError(nativeError)));
    return true;
}
} // namespace

UpdatePreparedPackage validatePreparedUpdatePackage(const QString &rootDirectory, const QString &expectedVersion,
                                                   const std::atomic_bool &cancelled, const UpdatePackageProgress &progress)
{
    UpdatePreparedPackage result;
    std::vector<updater::File> files;
    if (!loadManifest(rootDirectory, expectedVersion, cancelled, files, &result.error, progress, true)) return result;
    result.rootDirectory = QDir::cleanPath(rootDirectory); result.version = expectedVersion; return result;
}

UpdatePreparedPackage prepareUpdatePackage(const UpdatePackageRequest &request, const std::atomic_bool &cancelled,
                                          const UpdatePackageProgress &progress)
{
    UpdatePreparedPackage result;
    const auto reject = [&result](const QString &error) { result.error = error; return result; };
    if (!SemanticVersion::parse(request.expectedVersion) || !safe(request.archivePath) || !safe(request.workingDirectory)
        || QFileInfo::exists(request.workingDirectory) || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(QString::fromLatin1(request.expectedSha256)).hasMatch())
        return reject(QStringLiteral("更新包参数无效或准备目录已存在。"));
#ifdef Q_OS_WIN
    struct ArchiveLock {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~ArchiveLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } archiveLock;
    archiveLock.handle = CreateFileW(nativePath(request.archivePath).c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (archiveLock.handle == INVALID_HANDLE_VALUE) return reject(QStringLiteral("无法锁定待校验的更新 ZIP。"));
#endif
    const auto digest = hash(request.archivePath, cancelled, &result.error);
    if (digest.size() != 32 || digest.toHex() != request.expectedSha256.toLower())
        return reject(cancelled.load() ? QStringLiteral("更新准备已取消。") : QStringLiteral("更新 ZIP 的 SHA-256 校验失败。"));
    const auto inspection = inspectChickenRiceArchive(request.archivePath, cancelled);
    if (!inspection.success()) return reject(QStringLiteral("更新 ZIP 结构不安全：%1").arg(inspection.error));
    if (inspection.entries.size() > MaximumFiles + 1000 || inspection.unpackedBytes > MaximumTreeBytes)
        return reject(QStringLiteral("更新包展开后过大或文件数过多。"));
    const auto top = QStringLiteral("Arcanblade-") + request.expectedVersion + QStringLiteral("-windows-x64");
    const auto prefix = top + QLatin1Char('/');
    for (const auto &entry : inspection.entries) {
        if (entry.path == prefix && entry.directory) continue;
        if (!entry.path.startsWith(prefix) || entry.size > MaximumFileBytes)
            return reject(QStringLiteral("更新包的顶层目录、版本或文件大小不匹配。"));
        auto relative = entry.path.mid(prefix.size());
        if (entry.directory && relative.endsWith(QLatin1Char('/'))) relative.chop(1);
        if (!safeRelative(relative)) return reject(QStringLiteral("更新包存在不安全或用户数据文件。"));
    }
    auto parent = QFileInfo(request.workingDirectory).absolutePath();
    while (!QFileInfo::exists(parent) && parent != QFileInfo(parent).absolutePath()) parent = QFileInfo(parent).absolutePath();
    const QStorageInfo storage(parent);
    if (!storage.isValid() || storage.bytesAvailable() < inspection.unpackedBytes + 64 * 1024 * 1024)
        return reject(QStringLiteral("磁盘空间不足，无法展开更新包。"));
    if (!QDir().mkpath(request.workingDirectory) || !safe(request.workingDirectory)) return reject(QStringLiteral("无法创建安全的更新准备目录。"));
    const auto tar = systemTar();
    if (tar.isEmpty() || !QFileInfo(tar).isFile()) return reject(QStringLiteral("未找到 Windows 系统解压工具，请手动下载更新。"));
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    process.setProgram(tar);
    process.setArguments({QStringLiteral("-xf"), request.archivePath, QStringLiteral("-C"), request.workingDirectory});
    process.setStandardInputFile(QProcess::nullDevice());
    process.start();
    QElapsedTimer timer; timer.start();
    QByteArray diagnostic;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        diagnostic += process.readAllStandardError(); diagnostic += process.readAllStandardOutput();
        if (cancelled.load() || timer.elapsed() > 10 * 60 * 1000 || diagnostic.size() > 65536) {
            process.kill(); process.waitForFinished(5000);
            return reject(cancelled.load() ? QStringLiteral("更新准备已取消。") : QStringLiteral("展开更新包超时或输出异常。"));
        }
    }
    diagnostic += process.readAllStandardError();
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !diagnostic.trimmed().isEmpty())
        return reject(QStringLiteral("展开更新包失败：%1").arg(QString::fromLocal8Bit(diagnostic.left(2048))));
    if (cancelled.load()) return reject(QStringLiteral("更新准备已取消。"));
    const auto children = QDir(request.workingDirectory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    if (children != QStringList{top}) return reject(QStringLiteral("更新包存在额外的顶层文件。"));
    return validatePreparedUpdatePackage(QDir(request.workingDirectory).filePath(top), request.expectedVersion, cancelled, progress);
}

UpdateInstallPreparation prepareUpdateInstall(const QString &preparedRoot, const QString &targetRoot,
                                            const QString &currentVersion, const QString &newVersion,
                                            const QString &sessionDirectory, const std::atomic_bool &cancelled,
                                            const QByteArray &expectedManifestSha256)
{
    UpdateInstallPreparation result;
    const auto old = SemanticVersion::parse(currentVersion), next = SemanticVersion::parse(newVersion);
    if (!old || !next || SemanticVersion::compare(*next, *old) <= 0 || !safe(sessionDirectory) || QFileInfo::exists(sessionDirectory)
        || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(QString::fromLatin1(expectedManifestSha256)).hasMatch()) {
        result.error = QStringLiteral("更新版本无效或会话目录已存在。"); return result;
    }
    updater::Plan plan;
    if (!loadManifest(preparedRoot, newVersion, cancelled, plan.newFiles, &result.error, {}, true)) return result;
    const auto manifest = std::find_if(plan.newFiles.begin(), plan.newFiles.end(), [](const auto &file) { return file.path == "FILES.json"; });
    if (manifest == plan.newFiles.end() || QByteArray(reinterpret_cast<const char *>(manifest->sha256.data()), 32).toHex() != expectedManifestSha256.toLower()) {
        result.error = QStringLiteral("更新缓存与已验证的下载包不一致，请重新下载。"); return result;
    }
    if (!loadManifest(targetRoot, currentVersion, cancelled, plan.oldFiles, &result.error, {}, false)) return result;
    plan.targetDirectory = nativePath(targetRoot); plan.packageDirectory = nativePath(preparedRoot);
    plan.sessionDirectory = nativePath(sessionDirectory); plan.oldVersion = currentVersion.toStdString(); plan.newVersion = newVersion.toStdString();
    plan.parentPid = std::uint32_t(QCoreApplication::applicationPid()); plan.parentCreationTime = updater::currentProcessCreationTime();
    std::wstring nativeError;
    if (!updater::validatePlan(plan, &nativeError)) { result.error = textError(nativeError); return result; }
    if (cancelled.load() || !QDir().mkpath(sessionDirectory) || !safe(sessionDirectory)) { result.error = QStringLiteral("更新准备已取消或无法创建更新会话。"); return result; }
    if (!updater::preflight(plan, &nativeError)) { result.error = textError(nativeError); return result; }
    const auto helper = QDir(sessionDirectory).filePath(QStringLiteral("ArcanbladeUpdater.exe"));
    const auto source = QDir(targetRoot).filePath(QStringLiteral("ArcanbladeUpdater.exe"));
    const auto sourceDigest = hash(source, cancelled, &result.error);
    if (!QFile::copy(source, helper)) { result.error = QStringLiteral("无法复制更新器。"); return result; }
    const auto helperDigest = hash(helper, cancelled, &result.error);
    if (sourceDigest.size() != 32 || helperDigest.size() != 32 || sourceDigest != helperDigest || cancelled.load()) {
        result.error = QStringLiteral("无法准备更新器，或准备操作已取消。"); return result;
    }
    const auto planPath = QDir(sessionDirectory).filePath(QStringLiteral("install.plan"));
    if (!updater::writePlan(nativePath(planPath), plan, &nativeError)) { result.error = textError(nativeError); return result; }
    if (!marker(QFileInfo(preparedRoot).absolutePath(), QStringLiteral(".install-lease"), sessionDirectory.toUtf8())) {
        result.error = QStringLiteral("无法保留安装所需的更新缓存。"); return result;
    }
    result.helperPath = helper; result.planPath = planPath; result.sessionDirectory = sessionDirectory;
    return result;
}

bool launchPreparedUpdate(const UpdateInstallPreparation &preparation, const std::atomic_bool &cancelled, QString *error)
{
    updater::Plan plan;
    if (!loadPreparation(preparation, plan, error) || cancelled.load()) return fail(error, QStringLiteral("更新启动已取消或计划无效。"));
    const auto directory = preparation.sessionDirectory;
    for (const auto &name : {"ready", "proceed", "abort"}) if (QFileInfo::exists(QDir(directory).filePath(QString::fromLatin1(name))))
        return fail(error, QStringLiteral("此更新会话已启动过，请重新准备更新。"));
    QProcess process;
    process.setProgram(preparation.helperPath); process.setArguments({QStringLiteral("--plan"), preparation.planPath}); process.setWorkingDirectory(directory);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    if (!process.startDetached()) return fail(error, QStringLiteral("无法启动更新器，请从 GitHub 发布页手动下载。"));
    QElapsedTimer timer; timer.start();
    const auto expected = QByteArray::fromStdString(plan.newVersion);
    while (timer.elapsed() < 10000 && !cancelled.load()) {
        if (matchingMarker(directory, QStringLiteral("ready"), expected)) return true;
        if (QFileInfo::exists(QDir(directory).filePath(QStringLiteral("failed")))) break;
        QThread::msleep(25);
    }
    cancelPreparedUpdate(preparation);
    return fail(error, cancelled.load() ? QStringLiteral("更新启动已取消。") : QStringLiteral("更新器未能完成准备，请检查目录权限或使用手动下载。"));
}

bool commitPreparedUpdate(const UpdateInstallPreparation &preparation, QString *error)
{
    updater::Plan plan;
    if (!loadPreparation(preparation, plan, error) || plan.parentPid != std::uint32_t(QCoreApplication::applicationPid())
        || plan.parentCreationTime != updater::currentProcessCreationTime()
        || !matchingMarker(preparation.sessionDirectory, QStringLiteral("ready"), QByteArray::fromStdString(plan.newVersion))
        || QFileInfo::exists(QDir(preparation.sessionDirectory).filePath(QStringLiteral("abort")))
        || QFileInfo::exists(QDir(preparation.sessionDirectory).filePath(QStringLiteral("failed"))))
        return fail(error, QStringLiteral("更新器已取消、超时或尚未就绪。"));
    return marker(preparation.sessionDirectory, QStringLiteral("proceed"), QByteArray::fromStdString(plan.newVersion))
        || fail(error, QStringLiteral("无法确认重启更新。"));
}

void cancelPreparedUpdate(const UpdateInstallPreparation &preparation)
{
    if (!preparation.sessionDirectory.isEmpty() && safe(preparation.sessionDirectory)
        && marker(preparation.sessionDirectory, QStringLiteral("abort"), QByteArray("cancelled"))) {
        updater::Plan plan;
        // A failed launch carries an error but still owns the validated plan
        // and its cache lease. Keep every path/identity check when cancelling.
        auto cancelledPreparation = preparation;
        cancelledPreparation.error.clear();
        if (loadPreparation(cancelledPreparation, plan, nullptr) && plan.parentPid == std::uint32_t(QCoreApplication::applicationPid())
            && plan.parentCreationTime == updater::currentProcessCreationTime()) releaseInstallLease(plan);
    }
}

void confirmUpdateStartup(const QStringList &arguments, const QString &currentVersion)
{
    const auto flag = QStringLiteral("--arcanblade-update-session");
    const auto index = arguments.indexOf(flag);
    if (index < 0 || arguments.count(flag) != 1 || index + 1 >= arguments.size()) return;
    const auto session = QDir::cleanPath(arguments.at(index + 1));
    const auto cache = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("updates"));
    if (!safe(session) || !session.startsWith(cache + QLatin1Char('/'), Qt::CaseInsensitive)) return;
    updater::Plan plan;
    if (!updater::readPlan(nativePath(QDir(session).filePath(QStringLiteral("install.plan"))), plan)
        || QString::fromStdString(plan.newVersion) != currentVersion
        || textPath(plan.sessionDirectory).compare(session, Qt::CaseInsensitive) != 0
        || textPath(plan.targetDirectory / L"Arcanblade.exe").compare(QDir::cleanPath(QCoreApplication::applicationFilePath()), Qt::CaseInsensitive) != 0
        || !matchingMarker(session, QStringLiteral("proceed"), QByteArray::fromStdString(plan.newVersion))) return;
    if (marker(session, QStringLiteral("startup-confirmed"), QByteArray::fromStdString(plan.newVersion))) releaseInstallLease(plan);
}
} // namespace asmr
