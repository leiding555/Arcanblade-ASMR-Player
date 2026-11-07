#include "ai/FasterWhisperArchive.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringDecoder>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
constexpr qsizetype MaximumListingBytes = 32 * 1024 * 1024;
constexpr qsizetype MaximumEntries = 100000;
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
bool reparsePoint(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction()) return true;
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(info.absoluteFilePath());
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}
bool safeAncestors(const QString &path)
{
    QString current = QDir::cleanPath(path);
    if (!QDir::isAbsolutePath(current) || current.contains(QChar::Null)) return false;
    while (true) {
        if (reparsePoint(current)) return false;
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
bool safeRelativePath(const QString &path, bool directory)
{
    if (path.isEmpty() || path.size() > 2048 || path.startsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\')) || path.contains(QChar::Null)) return false;
    QString checked = path;
    if (directory && checked.endsWith(QLatin1Char('/'))) checked.chop(1);
    const auto components = checked.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const auto &component : components) {
        if (component.isEmpty() || component == QStringLiteral(".") || component == QStringLiteral("..")
            || component.size() > 255 || component.endsWith(QLatin1Char('.'))
            || component.endsWith(QLatin1Char(' '))) return false;
        for (const QChar c : component)
            if (c.unicode() < 32 || c.unicode() == 127 || QStringLiteral(":*?\"<>|").contains(c)) return false;
        const auto stem = component.section(QLatin1Char('.'), 0, 0).toUpper();
        if (stem == QStringLiteral("CON") || stem == QStringLiteral("PRN") || stem == QStringLiteral("AUX")
            || stem == QStringLiteral("NUL") || stem == QStringLiteral("CONIN$")
            || stem == QStringLiteral("CONOUT$")) return false;
        if ((stem.startsWith(QStringLiteral("COM")) || stem.startsWith(QStringLiteral("LPT")))
            && stem.size() == 4) {
            const auto n = stem.at(3).unicode();
            if ((n >= '1' && n <= '9') || n == 0xb9 || n == 0xb2 || n == 0xb3) return false;
        }
    }
    return true;
}
QString key(QString path)
{
    if (path.endsWith(QLatin1Char('/'))) path.chop(1);
    return path.normalized(QString::NormalizationForm_C).toCaseFolded();
}
bool safeFile(const QString &root, const QString &relative, QString *error)
{
    if (!safeRelativePath(relative, false) || !QFileInfo(root).isDir() || !safeAncestors(root))
        return fail(error, QStringLiteral("Missing or unsafe component directory."));
    QString current = QDir::cleanPath(root);
    const auto parts = relative.split(QLatin1Char('/'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
        current = QDir(current).filePath(parts.at(i));
        const QFileInfo info(current);
        if (!info.exists() || reparsePoint(current)
            || (i + 1 < parts.size() ? !info.isDir() : (!info.isFile() || info.size() <= 0)))
            return fail(error, QStringLiteral("Missing, empty, or unsafe component file: %1").arg(relative));
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
bool tarListing(const QString &archive, bool verbose, const std::atomic_bool &cancelled,
                QByteArray &output, QString *error)
{
    const auto program = systemTar();
    if (program.isEmpty() || !QFileInfo(program).isFile())
        return fail(error, QStringLiteral("System tar with 7z support was not found."));
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    process.setProgram(program);
    process.setArguments({verbose ? QStringLiteral("-tvf") : QStringLiteral("-tf"), archive});
    process.setStandardInputFile(QProcess::nullDevice());
    process.start();
    QElapsedTimer clock;
    clock.start();
    QByteArray diagnostic;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        output += process.readAllStandardOutput();
        diagnostic += process.readAllStandardError();
        if (cancelled.load() || clock.elapsed() > 120000 || output.size() > MaximumListingBytes
            || diagnostic.size() > 64 * 1024) {
            process.kill();
            process.waitForFinished(5000);
            return fail(error, cancelled.load() ? QStringLiteral("Archive inspection cancelled.")
                : QStringLiteral("Archive listing exceeded its time or output limit."));
        }
    }
    output += process.readAllStandardOutput();
    diagnostic += process.readAllStandardError();
    if (cancelled.load()) return fail(error, QStringLiteral("Archive inspection cancelled."));
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0 || output.size() > MaximumListingBytes || !diagnostic.trimmed().isEmpty())
        return fail(error, QStringLiteral("System tar could not safely list this 7z archive: %1")
            .arg(QString::fromLocal8Bit(diagnostic.left(2048)).trimmed()));
    return true;
}
} // namespace

FasterWhisperArchiveInspection inspectFasterWhisperArchive(
    const QString &path, const std::atomic_bool &cancelled, qint64 maximumUnpackedBytes)
{
    FasterWhisperArchiveInspection result;
    const auto reject = [&result](const QString &message) {
        result.entries.clear(); result.unpackedBytes = 0; result.error = message; return result;
    };
    if (cancelled.load()) return reject(QStringLiteral("Archive inspection cancelled."));
    const QFileInfo before(path);
    QFile archive(path);
    if (maximumUnpackedBytes <= 0 || !before.isFile() || !safeAncestors(before.absoluteFilePath())
        || !archive.open(QIODevice::ReadOnly) || archive.read(6) != QByteArray::fromHex("377abcaf271c"))
        return reject(QStringLiteral("Cannot read a regular, supported 7z archive."));
    archive.close();
    QByteArray namesBytes, verboseBytes;
    QString error;
    if (!tarListing(path, false, cancelled, namesBytes, &error)
        || !tarListing(path, true, cancelled, verboseBytes, &error)) return reject(error);
    const auto after = QFileInfo(path);
    if (!after.isFile() || !safeAncestors(after.absoluteFilePath()) || before.size() != after.size()
        || before.lastModified() != after.lastModified())
        return reject(QStringLiteral("Archive changed during inspection."));
    auto names = namesBytes.split('\n');
    auto verbose = verboseBytes.split('\n');
    if (!names.isEmpty() && names.last().isEmpty()) names.removeLast();
    if (!verbose.isEmpty() && verbose.last().isEmpty()) verbose.removeLast();
    if (names.isEmpty() || names.size() > MaximumEntries || names.size() != verbose.size())
        return reject(QStringLiteral("Archive listing is empty, oversized, or inconsistent."));
    QSet<QString> seen;
    const QRegularExpression metadata(QStringLiteral(
        "^([d-])[rwxstST-]{9}[+@]?\\s+\\d+\\s+\\S+\\s+\\S+\\s+(\\d+)\\s+.+\\s+$"));
    for (qsizetype i = 0; i < names.size(); ++i) {
        if (cancelled.load()) return reject(QStringLiteral("Archive inspection cancelled."));
        auto rawName = names.at(i);
        auto rawVerbose = verbose.at(i);
        if (rawName.endsWith('\r')) rawName.chop(1);
        if (rawVerbose.endsWith('\r')) rawVerbose.chop(1);
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString name = decoder.decode(rawName);
        if (decoder.hasError() || rawVerbose.size() <= rawName.size()
            || !rawVerbose.endsWith(rawName) || rawVerbose.at(rawVerbose.size() - rawName.size() - 1) != ' ')
            return reject(QStringLiteral("Unsupported or ambiguous archive filename encoding."));
        const QString prefix = QString::fromLocal8Bit(rawVerbose.left(rawVerbose.size() - rawName.size()));
        const auto match = metadata.match(prefix);
        if (!match.hasMatch()) return reject(QStringLiteral("Unsupported archive member type or listing metadata."));
        const bool directory = match.captured(1) == QStringLiteral("d");
        bool numberOk = false;
        const qint64 size = match.captured(2).toLongLong(&numberOk);
        if (!numberOk || size < 0 || (directory && size != 0) || !safeRelativePath(name, directory)
            || seen.contains(key(name)) || size > maximumUnpackedBytes - result.unpackedBytes)
            return reject(QStringLiteral("Unsafe, duplicate, or oversized archive entry."));
        seen.insert(key(name));
        result.entries.append({name, size, directory});
        result.unpackedBytes += size;
    }
    QHash<QString, bool> paths;
    for (const auto &entry : result.entries) paths.insert(key(entry.path), entry.directory);
    for (const auto &entry : result.entries) {
        QString parent = key(entry.path).section(QLatin1Char('/'), 0, -2);
        while (!parent.isEmpty()) {
            if (paths.contains(parent) && !paths.value(parent))
                return reject(QStringLiteral("Archive file conflicts with an ancestor directory."));
            const auto at = parent.lastIndexOf(QLatin1Char('/'));
            parent = at < 0 ? QString() : parent.left(at);
        }
    }
    return result;
}

bool validateFasterWhisperExtracted(const QString &staging,
    const FasterWhisperArchiveInspection &inspection, const std::atomic_bool &cancelled, QString *error)
{
    if (error) error->clear();
    if (!inspection.success() || !QFileInfo(staging).isDir() || !safeAncestors(staging))
        return fail(error, QStringLiteral("Invalid or unsafe extraction staging directory."));
    QHash<QString, FasterWhisperArchiveEntry> expected;
    QSet<QString> directories;
    for (const auto &entry : inspection.entries) {
        if (!safeRelativePath(entry.path, entry.directory) || expected.contains(key(entry.path)))
            return fail(error, QStringLiteral("Invalid extraction manifest."));
        expected.insert(key(entry.path), entry);
        QString parent = key(entry.path);
        if (!entry.directory) parent = parent.section(QLatin1Char('/'), 0, -2);
        while (!parent.isEmpty()) {
            directories.insert(parent);
            const auto at = parent.lastIndexOf(QLatin1Char('/'));
            parent = at < 0 ? QString() : parent.left(at);
        }
    }
    QSet<QString> found;
    QDirIterator iterator(staging, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    qsizetype count = 0;
    while (iterator.hasNext()) {
        if (cancelled.load()) return fail(error, QStringLiteral("Extraction verification cancelled."));
        const auto absolute = iterator.next();
        const auto info = iterator.fileInfo();
        const auto relative = QDir(staging).relativeFilePath(absolute);
        const auto entryKey = key(relative);
        if (++count > MaximumEntries * 4 || reparsePoint(absolute) || !safeAncestors(absolute)
            || !safeRelativePath(relative, info.isDir()))
            return fail(error, QStringLiteral("Unsafe path in extracted component."));
        if (info.isDir()) {
            if (!directories.contains(entryKey)) return fail(error, QStringLiteral("Unexpected extracted directory."));
        } else {
            if (!info.isFile() || !expected.contains(entryKey) || expected.value(entryKey).directory
                || expected.value(entryKey).size != info.size())
                return fail(error, QStringLiteral("Unexpected or incomplete extracted file: %1").arg(relative));
        }
        found.insert(entryKey);
    }
    for (auto i = expected.cbegin(); i != expected.cend(); ++i)
        if (!found.contains(i.key())) return fail(error, QStringLiteral("An archive member was not extracted."));
    return true;
}

QString findFasterWhisperEngineRoot(const QString &staging, QString *error)
{
    if (error) error->clear();
    if (!QFileInfo(staging).isDir() || !safeAncestors(staging)) {
        fail(error, QStringLiteral("Missing or unsafe extraction directory.")); return {};
    }
    QStringList candidates;
    if (QFileInfo(QDir(staging).filePath(QStringLiteral("faster-whisper-xxl.exe"))).isFile()) candidates.append(staging);
    const auto children = QDir(staging).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const auto &child : children)
        if (!reparsePoint(child.absoluteFilePath())
            && QFileInfo(QDir(child.absoluteFilePath()).filePath(QStringLiteral("faster-whisper-xxl.exe"))).isFile())
            candidates.append(child.absoluteFilePath());
    if (candidates.size() != 1) {
        fail(error, QStringLiteral("Expected exactly one Faster-Whisper-XXL component directory.")); return {};
    }
    if (!validateFasterWhisperEngine(candidates.first(), error)) return {};
    return QDir::cleanPath(candidates.first());
}

bool validateFasterWhisperEngine(const QString &root, QString *error)
{
    if (error) error->clear();
    const QStringList required{
        QStringLiteral("faster-whisper-xxl.exe"), QStringLiteral("ffmpeg.exe"),
        QStringLiteral("_xxl_data/python310.dll"), QStringLiteral("_xxl_data/base_library.zip"),
        QStringLiteral("_xxl_data/ctranslate2/_ext.cp310-win_amd64.pyd"),
        QStringLiteral("_xxl_data/ctranslate2/ctranslate2.dll"),
        QStringLiteral("_xxl_data/faster_whisper/assets/silero_vad.onnx")};
    for (const auto &relative : required)
        if (!safeFile(root, relative, error)) return false;
    return true;
}

bool validateFasterWhisperModel(const QString &root, const QList<FasterWhisperDownloadFile> &files,
    const std::atomic_bool &cancelled, QString *error,
    const std::function<void(qint64, qint64)> &progress)
{
    if (error) error->clear();
    if (cancelled.load()) return fail(error, QStringLiteral("Model verification cancelled."));
    if (files.size() != 4) return fail(error, QStringLiteral("Invalid large-v2 model manifest."));
    const QSet<QString> required{QStringLiteral("config.json"), QStringLiteral("model.bin"),
        QStringLiteral("tokenizer.json"), QStringLiteral("vocabulary.txt")};
    QSet<QString> seen;
    qint64 total = 0;
    for (const auto &entry : files) {
        if (!required.contains(entry.name) || seen.contains(entry.name) || entry.sha256.size() != 64
            || entry.size <= 0 || entry.size > std::numeric_limits<qint64>::max() - total)
            return fail(error, QStringLiteral("Invalid large-v2 model manifest entry."));
        seen.insert(entry.name); total += entry.size;
    }
    qint64 completed = 0;
    for (const auto &entry : files) {
        if (!safeFile(root, entry.name, error)) return false;
        const auto path = QDir(root).filePath(entry.name);
        const QFileInfo before(path);
        QFile file(path);
        if (before.size() != entry.size || !file.open(QIODevice::ReadOnly))
            return fail(error, QStringLiteral("Model file has the wrong size or cannot be read: %1").arg(entry.name));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            if (cancelled.load()) return fail(error, QStringLiteral("Model verification cancelled."));
            const auto bytes = file.read(1024 * 1024);
            if (bytes.isEmpty() && file.error() != QFileDevice::NoError)
                return fail(error, QStringLiteral("Could not read model file: %1").arg(entry.name));
            hash.addData(bytes); completed += bytes.size();
            if (progress) progress(completed, total);
        }
        file.close();
        const QFileInfo after(path);
        if (!safeAncestors(path) || after.size() != before.size() || after.lastModified() != before.lastModified()
            || hash.result().toHex() != entry.sha256)
            return fail(error, QStringLiteral("Model SHA-256 verification failed: %1").arg(entry.name));
        if (entry.name.endsWith(QStringLiteral(".json"))) {
            if (entry.size > 16 * 1024 * 1024 || !file.open(QIODevice::ReadOnly))
                return fail(error, QStringLiteral("Cannot read bounded model metadata."));
            const auto document = QJsonDocument::fromJson(file.readAll());
            if (!document.isObject() || document.object().isEmpty())
                return fail(error, QStringLiteral("Invalid model JSON: %1").arg(entry.name));
        }
    }
    if (cancelled.load()) return fail(error, QStringLiteral("Model verification cancelled."));
    return true;
}
} // namespace asmr
