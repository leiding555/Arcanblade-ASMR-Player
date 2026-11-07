#include "ai/ChickenRiceArchive.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
constexpr quint32 LocalSignature = 0x04034b50;
constexpr quint32 CentralSignature = 0x02014b50;
constexpr quint32 EndSignature = 0x06054b50;
constexpr quint32 Zip64EndSignature = 0x06064b50;
constexpr quint32 Zip64LocatorSignature = 0x07064b50;
constexpr quint32 DescriptorSignature = 0x08074b50;
constexpr quint64 MaximumEntries = 100000;
constexpr quint64 MaximumCentralBytes = 256ULL * 1024 * 1024;
constexpr quint64 SignedMaximum = quint64(std::numeric_limits<qint64>::max());

quint16 u16(const QByteArray &bytes, qsizetype at)
{ return qFromLittleEndian<quint16>(bytes.constData() + at); }
quint32 u32(const QByteArray &bytes, qsizetype at)
{ return qFromLittleEndian<quint32>(bytes.constData() + at); }
quint64 u64(const QByteArray &bytes, qsizetype at)
{ return qFromLittleEndian<quint64>(bytes.constData() + at); }

bool within(quint64 at, quint64 length, quint64 limit)
{ return at <= limit && length <= limit - at; }

QByteArray readAt(QFile &file, quint64 at, qint64 length)
{
    if (length < 0 || !within(at, quint64(length), quint64(file.size()))
        || at > SignedMaximum || !file.seek(qint64(at)))
        return {};
    const auto bytes = file.read(length);
    return bytes.size() == length ? bytes : QByteArray();
}

bool setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

struct ExtraFields {
    QByteArray zip64;
    bool hasZip64 = false;
};

// Reject extras that can replace a checked name or encode a Unix link target.
// Ordinary timestamp/NTFS timestamp fields are harmless metadata.
bool readExtras(const QByteArray &bytes, ExtraFields &fields)
{
    qsizetype at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 4)
            return false;
        const auto tag = u16(bytes, at);
        const auto length = u16(bytes, at + 2);
        at += 4;
        if (bytes.size() - at < length)
            return false;
        if (tag == 0x7075 || tag == 0x000d || tag == 0x756e || tag == 0x9901)
            return false;
        if (tag == 1) {
            if (fields.hasZip64)
                return false;
            fields.hasZip64 = true;
            fields.zip64 = bytes.mid(at, length);
        }
        at += length;
    }
    return true;
}

bool take64(const QByteArray &bytes, qsizetype &at, quint64 &value)
{
    if (bytes.size() - at < 8)
        return false;
    value = u64(bytes, at);
    at += 8;
    return true;
}

bool safeArchivePath(const QString &path, bool directory)
{
    if (path.isEmpty() || path.size() > 2048 || path.startsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\')) || path.contains(QChar::Null))
        return false;
    QString checked = path;
    if (directory && checked.endsWith(QLatin1Char('/')))
        checked.chop(1);
    if (checked.isEmpty() || checked.endsWith(QLatin1Char('/')))
        return false;
    const auto components = checked.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const auto &component : components) {
        if (component.isEmpty() || component == QStringLiteral(".")
            || component == QStringLiteral("..") || component.size() > 255
            || component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' ')))
            return false;
        for (const QChar character : component) {
            if (character.unicode() < 32 || character.unicode() == 127
                || QStringLiteral(":*?\"<>|").contains(character))
                return false;
        }
        const QString stem = component.section(QLatin1Char('.'), 0, 0).toUpper();
        if (stem == QStringLiteral("CON") || stem == QStringLiteral("PRN")
            || stem == QStringLiteral("AUX") || stem == QStringLiteral("NUL")
            || stem == QStringLiteral("CONIN$") || stem == QStringLiteral("CONOUT$"))
            return false;
        if ((stem.startsWith(QStringLiteral("COM")) || stem.startsWith(QStringLiteral("LPT")))
            && stem.size() == 4) {
            const auto number = stem.at(3).unicode();
            if ((number >= '1' && number <= '9') || number == 0x00b9
                || number == 0x00b2 || number == 0x00b3)
                return false;
        }
    }
    return true;
}

QString pathKey(QString path)
{
    if (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path.normalized(QString::NormalizationForm_C).toCaseFolded();
}

bool decodeName(const QByteArray &bytes, quint16 flags, QString &name)
{
    if (flags & 0x0800) {
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        name = decoder.decode(bytes);
        return !decoder.hasError();
    }
    // The pinned distribution uses UTF-8 for non-ASCII names. Reject legacy
    // encodings rather than checking one spelling and extracting another.
    for (const char character : bytes) {
        if (quint8(character) >= 128)
            return false;
    }
    name = QString::fromLatin1(bytes);
    return true;
}

bool reparsePoint(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction())
        return true;
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(info.absoluteFilePath());
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool safeExistingPath(const QString &root, const QString &relative, bool directory,
                      QString *error)
{
    QString current = QDir(root).absolutePath();
    if (!QFileInfo(current).isDir() || reparsePoint(current))
        return setError(error, QStringLiteral("Engine root is missing or is a reparse point."));
    const auto components = relative.split(QLatin1Char('/'));
    for (qsizetype index = 0; index < components.size(); ++index) {
        current = QDir(current).filePath(components.at(index));
        const QFileInfo info(current);
        const bool wantDirectory = index + 1 < components.size() || directory;
        if (!info.exists() || reparsePoint(current)
            || (wantDirectory ? !info.isDir() : (!info.isFile() || info.size() <= 0)))
            return setError(error, QStringLiteral("Missing, empty, or unsafe engine path: %1").arg(relative));
    }
    return true;
}

bool readJson(const QString &root, const QString &relative, QJsonDocument &document,
              QString *error)
{
    if (!safeExistingPath(root, relative, false, error))
        return false;
    QFile file(QDir(root).filePath(relative));
    if (file.size() > 16 * 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return setError(error, QStringLiteral("Cannot read bounded engine JSON: %1").arg(relative));
    const auto bytes = file.readAll();
    QJsonParseError parseError;
    document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || document.isNull()
        || (document.isObject() && document.object().isEmpty())
        || (document.isArray() && document.array().isEmpty()))
        return setError(error, QStringLiteral("Invalid or empty engine JSON: %1").arg(relative));
    return true;
}

struct EntryRange {
    quint64 start;
    quint64 end;
};
} // namespace

ChickenRiceArchiveInspection inspectChickenRiceArchive(
    const QString &path, const std::atomic_bool &cancelled)
{
    ChickenRiceArchiveInspection result;
    const auto fail = [&result](const QString &error) {
        result.entries.clear();
        result.unpackedBytes = 0;
        result.error = error;
        return result;
    };
    if (cancelled.load())
        return fail(QStringLiteral("Archive inspection cancelled."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 22)
        return fail(QStringLiteral("Cannot read a complete ZIP archive."));
    const quint64 fileSize = quint64(file.size());
    const qint64 tailLength = qint64(std::min<quint64>(fileSize, 22 + 65535));
    const auto tail = readAt(file, fileSize - quint64(tailLength), tailLength);
    qsizetype endIndex = -1;
    for (qsizetype at = tail.size() - 22; at >= 0; --at) {
        if (u32(tail, at) == EndSignature && at + 22 + u16(tail, at + 20) == tail.size()) {
            endIndex = at;
            break;
        }
    }
    if (endIndex < 0)
        return fail(QStringLiteral("ZIP end record is missing or has trailing bytes."));
    const auto end = tail.mid(endIndex, 22);
    const quint64 endOffset = fileSize - quint64(tailLength) + quint64(endIndex);
    if (u16(end, 4) != 0 || u16(end, 6) != 0)
        return fail(QStringLiteral("Multi-disk ZIP archives are unsupported."));
    quint64 count = u16(end, 10);
    quint64 centralBytes = u32(end, 12);
    quint64 centralOffset = u32(end, 16);
    quint64 centralLimit = endOffset;
    const bool zip64 = count == 0xffff || u16(end, 8) == 0xffff
        || centralBytes == 0xffffffff || centralOffset == 0xffffffff;
    if (zip64) {
        if (endOffset < 20)
            return fail(QStringLiteral("ZIP64 locator is missing."));
        const auto locator = readAt(file, endOffset - 20, 20);
        if (locator.size() != 20 || u32(locator, 0) != Zip64LocatorSignature
            || u32(locator, 4) != 0 || u32(locator, 16) != 1)
            return fail(QStringLiteral("Invalid or multi-disk ZIP64 locator."));
        const quint64 zip64Offset = u64(locator, 8);
        const auto record = readAt(file, zip64Offset, 56);
        if (record.size() != 56 || u32(record, 0) != Zip64EndSignature
            || u64(record, 4) < 44 || u64(record, 4) > 1024 * 1024
            || !within(zip64Offset, 12 + u64(record, 4), endOffset - 20)
            || zip64Offset + 12 + u64(record, 4) != endOffset - 20
            || u32(record, 16) != 0 || u32(record, 20) != 0
            || u64(record, 24) != u64(record, 32))
            return fail(QStringLiteral("Invalid ZIP64 end record."));
        count = u64(record, 32);
        centralBytes = u64(record, 40);
        centralOffset = u64(record, 48);
        centralLimit = zip64Offset;
        if ((u16(end, 8) != 0xffff && u16(end, 8) != count)
            || (u16(end, 10) != 0xffff && u16(end, 10) != count)
            || (u32(end, 12) != 0xffffffff && u32(end, 12) != centralBytes)
            || (u32(end, 16) != 0xffffffff && u32(end, 16) != centralOffset))
            return fail(QStringLiteral("ZIP and ZIP64 end records disagree."));
    } else if (u16(end, 8) != count) {
        return fail(QStringLiteral("ZIP entry counts disagree."));
    }
    if (count == 0 || count > MaximumEntries || centralBytes > MaximumCentralBytes
        || count > centralBytes / 46 || !within(centralOffset, centralBytes, centralLimit)
        || centralOffset + centralBytes != centralLimit)
        return fail(QStringLiteral("Invalid ZIP central directory bounds or entry count."));
    const quint64 centralEnd = centralOffset + centralBytes;
    quint64 cursor = centralOffset;
    quint64 unpacked = 0;
    QHash<QString, bool> paths;
    QSet<QString> requiredDirectories;
    QList<EntryRange> ranges;
    result.entries.reserve(qsizetype(count));
    ranges.reserve(qsizetype(count));
    for (quint64 index = 0; index < count; ++index) {
        if (cancelled.load())
            return fail(QStringLiteral("Archive inspection cancelled."));
        if (!within(cursor, 46, centralEnd))
            return fail(QStringLiteral("Truncated ZIP central directory."));
        const auto header = readAt(file, cursor, 46);
        if (header.size() != 46 || u32(header, 0) != CentralSignature)
            return fail(QStringLiteral("Invalid ZIP central entry signature."));
        const quint16 flags = u16(header, 8);
        const quint16 method = u16(header, 10);
        if ((flags & ~quint16(0x080e)) != 0 || (method != 0 && method != 8))
            return fail(QStringLiteral("Encrypted or unsupported ZIP entry."));
        const quint64 nameBytes = u16(header, 28);
        const quint64 extraBytes = u16(header, 30);
        const quint64 commentBytes = u16(header, 32);
        const quint64 entryLength = 46 + nameBytes + extraBytes + commentBytes;
        if (nameBytes == 0 || !within(cursor, entryLength, centralEnd))
            return fail(QStringLiteral("Truncated ZIP entry name or extra fields."));
        const auto rawName = readAt(file, cursor + 46, qint64(nameBytes));
        const auto rawExtra = readAt(file, cursor + 46 + nameBytes, qint64(extraBytes));
        ExtraFields extras;
        if (rawName.size() != qint64(nameBytes) || rawExtra.size() != qint64(extraBytes))
            return fail(QStringLiteral("Cannot read complete ZIP entry metadata."));
        if (!readExtras(rawExtra, extras))
            return fail(QStringLiteral("Unsafe or malformed ZIP extra field."));
        quint64 compressed = u32(header, 20);
        quint64 size = u32(header, 24);
        quint64 localOffset = u32(header, 42);
        quint64 disk = u16(header, 34);
        qsizetype extraAt = 0;
        const bool needSize = size == 0xffffffff;
        const bool needCompressed = compressed == 0xffffffff;
        const bool needOffset = localOffset == 0xffffffff;
        if ((needSize && (!extras.hasZip64 || !take64(extras.zip64, extraAt, size)))
            || (needCompressed && (!extras.hasZip64 || !take64(extras.zip64, extraAt, compressed)))
            || (needOffset && (!extras.hasZip64 || !take64(extras.zip64, extraAt, localOffset))))
            return fail(QStringLiteral("Missing ZIP64 entry sizes or offset."));
        if (disk == 0xffff) {
            if (!extras.hasZip64 || extras.zip64.size() - extraAt < 4)
                return fail(QStringLiteral("Missing ZIP64 entry disk."));
            disk = u32(extras.zip64, extraAt);
        }
        if (disk != 0 || size > SignedMaximum || compressed > SignedMaximum
            || size > SignedMaximum - unpacked)
            return fail(QStringLiteral("ZIP entry size overflow or multi-disk entry."));
        QString name;
        if (!decodeName(rawName, flags, name))
            return fail(QStringLiteral("ZIP entry name is not unambiguous UTF-8 or ASCII."));
        const quint32 attributes = u32(header, 38);
        const quint32 unixType = (attributes >> 16) & 0170000;
        const bool directory = name.endsWith(QLatin1Char('/'));
        if ((attributes & 0x400) != 0
            || (unixType != 0 && unixType != 0100000 && unixType != 0040000)
            || (unixType == 0040000 && !directory)
            || (unixType == 0100000 && directory)
            || ((attributes & 0x10) != 0 && !directory))
            return fail(QStringLiteral("ZIP links, reparse points, or special files are forbidden."));
        if (!safeArchivePath(name, directory) || (directory && (size != 0 || compressed != 0))
            || (method == 0 && size != compressed))
            return fail(QStringLiteral("Unsafe ZIP path or inconsistent stored entry."));
        const QString key = pathKey(name);
        if (paths.contains(key) || (!directory && requiredDirectories.contains(key)))
            return fail(QStringLiteral("Duplicate or conflicting ZIP entry path."));
        QString parent = key;
        while (parent.contains(QLatin1Char('/'))) {
            parent = parent.left(parent.lastIndexOf(QLatin1Char('/')));
            if (paths.contains(parent) && !paths.value(parent))
                return fail(QStringLiteral("ZIP file is used as a parent directory."));
            requiredDirectories.insert(parent);
        }
        paths.insert(key, directory);
        if (!within(localOffset, 30, centralOffset))
            return fail(QStringLiteral("ZIP local entry offset is outside the data region."));
        const auto local = readAt(file, localOffset, 30);
        if (local.size() != 30 || u32(local, 0) != LocalSignature
            || u16(local, 6) != flags || u16(local, 8) != method
            || u16(local, 26) != nameBytes)
            return fail(QStringLiteral("ZIP local and central headers disagree."));
        const quint64 localExtraBytes = u16(local, 28);
        const quint64 localLength = 30 + nameBytes + localExtraBytes;
        if (!within(localOffset, localLength, centralOffset)
            || readAt(file, localOffset + 30, qint64(nameBytes)) != rawName)
            return fail(QStringLiteral("ZIP local path differs from the checked central path."));
        ExtraFields localExtras;
        const auto localExtra = readAt(file, localOffset + 30 + nameBytes, qint64(localExtraBytes));
        if (localExtra.size() != qint64(localExtraBytes) || !readExtras(localExtra, localExtras))
            return fail(QStringLiteral("Unsafe ZIP local extra field."));
        quint64 localSize = u32(local, 22);
        quint64 localCompressed = u32(local, 18);
        const bool local64 = localSize == 0xffffffff || localCompressed == 0xffffffff;
        qsizetype localExtraAt = 0;
        if ((localSize == 0xffffffff
                && (!localExtras.hasZip64 || !take64(localExtras.zip64, localExtraAt, localSize)))
            || (localCompressed == 0xffffffff
                && (!localExtras.hasZip64 || !take64(localExtras.zip64, localExtraAt, localCompressed))))
            return fail(QStringLiteral("Missing ZIP64 local sizes."));
        const quint32 crc = u32(header, 16);
        const bool descriptor = (flags & 8) != 0;
        if ((!descriptor && (localSize != size || localCompressed != compressed || u32(local, 14) != crc))
            || (descriptor && ((localSize != 0 && localSize != size)
                || (localCompressed != 0 && localCompressed != compressed)
                || (u32(local, 14) != 0 && u32(local, 14) != crc))))
            return fail(QStringLiteral("ZIP local sizes or CRC disagree with central metadata."));
        const quint64 dataOffset = localOffset + localLength;
        if (!within(dataOffset, compressed, centralOffset))
            return fail(QStringLiteral("ZIP compressed data exceeds its data region."));
        quint64 rangeEnd = dataOffset + compressed;
        if (descriptor) {
            const bool wide = local64 || needSize || needCompressed;
            const qint64 payloadLength = wide ? 20 : 12;
            bool validDescriptor = false;
            for (const bool signature : {false, true}) {
                const qint64 length = payloadLength + (signature ? 4 : 0);
                if (!within(rangeEnd, quint64(length), centralOffset))
                    continue;
                const auto bytes = readAt(file, rangeEnd, length);
                const qsizetype offset = signature ? 4 : 0;
                if (signature && u32(bytes, 0) != DescriptorSignature)
                    continue;
                const quint64 descriptorCompressed = wide ? u64(bytes, offset + 4) : u32(bytes, offset + 4);
                const quint64 descriptorSize = wide ? u64(bytes, offset + 12) : u32(bytes, offset + 8);
                if (u32(bytes, offset) == crc && descriptorCompressed == compressed && descriptorSize == size) {
                    rangeEnd += quint64(length);
                    validDescriptor = true;
                    break;
                }
            }
            if (!validDescriptor)
                return fail(QStringLiteral("Invalid ZIP data descriptor."));
        }
        ranges.append({localOffset, rangeEnd});
        result.entries.append({name, qint64(size), directory, crc});
        unpacked += size;
        cursor += entryLength;
    }
    if (cursor != centralEnd)
        return fail(QStringLiteral("ZIP central directory has unaccounted records."));
    std::sort(ranges.begin(), ranges.end(), [](const EntryRange &a, const EntryRange &b) {
        return a.start < b.start;
    });
    quint64 previousEnd = 0;
    for (const auto &range : ranges) {
        if (range.start != previousEnd)
            return fail(QStringLiteral("Overlapping ZIP entries or unaccounted data."));
        previousEnd = range.end;
    }
    if (previousEnd != centralOffset)
        return fail(QStringLiteral("ZIP data region does not match its directory."));
    if (cancelled.load())
        return fail(QStringLiteral("Archive inspection cancelled."));
    result.unpackedBytes = qint64(unpacked);
    return result;
}

QString findChickenRiceEngineRoot(const QString &staging, QString *error)
{
    if (error)
        error->clear();
    const QFileInfo info(staging);
    if (!info.isDir() || reparsePoint(staging)) {
        setError(error, QStringLiteral("Engine staging directory is missing or unsafe."));
        return {};
    }
    const QDir directory(info.absoluteFilePath());
    QStringList candidates;
    const auto add = [&candidates](const QString &candidate) {
        const QFileInfo engine(QDir(candidate).filePath(QStringLiteral("infer.exe")));
        if (engine.isFile() && engine.size() > 0 && !reparsePoint(engine.absoluteFilePath()))
            candidates.append(QDir(candidate).absolutePath());
    };
    add(directory.absolutePath());
    const auto children = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto &child : children) {
        if (!reparsePoint(child.absoluteFilePath()))
            add(child.absoluteFilePath());
    }
    if (candidates.size() != 1) {
        setError(error, QStringLiteral("Expected exactly one engine at staging root or one directory below."));
        return {};
    }
    return candidates.first();
}

bool validateChickenRiceEngine(const QString &root, QString *error)
{
    if (error)
        error->clear();
    if (!QDir::isAbsolutePath(root))
        return setError(error, QStringLiteral("Engine root must be absolute."));
    const QStringList binaryFiles{
        QStringLiteral("infer.exe"), QStringLiteral("_internal/python310.dll"),
        QStringLiteral("_internal/base_library.zip"), QStringLiteral("models/model.bin"),
        QStringLiteral("models/whisper_vad.onnx")};
    for (const auto &relative : binaryFiles) {
        if (!safeExistingPath(root, relative, false, error))
            return false;
    }
    const QStringList jsonFiles{
        QStringLiteral("models/config.json"), QStringLiteral("models/preprocessor_config.json"),
        QStringLiteral("models/tokenizer.json"), QStringLiteral("models/vocabulary.json"),
        QStringLiteral("models/whisper_vad_metadata.json"),
        QStringLiteral("models/whisper-base/config.json"),
        QStringLiteral("models/whisper-base/preprocessor_config.json"),
        QStringLiteral("models/whisper-base/tokenizer.json"),
        QStringLiteral("models/whisper-base/vocab.json")};
    for (const auto &relative : jsonFiles) {
        QJsonDocument document;
        if (!readJson(root, relative, document, error))
            return false;
        if (relative.endsWith(QStringLiteral("preprocessor_config.json"))
            && (!document.isObject()
                || document.object().value(QStringLiteral("sampling_rate")).toInt() != 16000
                || document.object().value(QStringLiteral("feature_size")).toInt() != 80))
            return setError(error, QStringLiteral("Invalid local Whisper feature configuration: %1").arg(relative));
    }
    return true;
}
} // namespace asmr
