#include "library/WorkReferenceExtractor.h"

#include "subtitle/LrcParser.h"
#include "subtitle/SrtParser.h"
#include "subtitle/SubtitleFileLoader.h"
#include "subtitle/VttParser.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPdfDocument>
#include <QPdfSelection>
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
QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool hashValid(const QString &value)
{
    if (value.size() != 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar ch) {
        return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'));
    });
}
bool supportedText(const QString &format)
{
    static const QSet<QString> formats{QStringLiteral("txt"), QStringLiteral("md"),
        QStringLiteral("markdown"), QStringLiteral("nfo"), QStringLiteral("log"),
        QStringLiteral("cue"), QStringLiteral("ks"), QStringLiteral("readme")};
    return formats.contains(format);
}
bool subtitleFormat(const QString &format)
{
    return format == QStringLiteral("srt") || format == QStringLiteral("vtt")
        || format == QStringLiteral("lrc");
}
bool safePath(QString path)
{
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)) return false;
    path = QDir::fromNativeSeparators(path);
    if (path.startsWith(QStringLiteral("//?/")) || path.startsWith(QStringLiteral("//./"))) return false;
    path = QDir::cleanPath(path);
    if (!QFileInfo(path).isFile()) return false;
    for (;;) {
        const QFileInfo info(path);
        if (!info.exists() || info.isSymbolicLink() || info.isJunction()) return false;
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        const auto parent = info.absolutePath();
        if (parent == path) return true;
        if (!QFileInfo(parent).isDir()) return false;
        path = parent;
    }
}
void normalize(QString &text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
}
// Counts scalar values without allocating a UCS-4 copy. Newlines have already
// been normalized; TAB and LF are the only admitted control characters.
bool validText(const QString &text, qsizetype *count = nullptr)
{
    qsizetype characters = 0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto ch = text.at(index);
        if (ch.isHighSurrogate()) {
            if (++index >= text.size() || !text.at(index).isLowSurrogate()) return false;
        } else if (ch.isLowSurrogate()) return false;
        else if (ch.category() == QChar::Other_Control && ch != QLatin1Char('\n') && ch != QLatin1Char('\t')) return false;
        ++characters;
    }
    if (count) *count = characters;
    return true;
}
bool decode(const QByteArray &bytes, QString &text, QString &encoding, bool &inferred)
{
    QStringConverter::Encoding type = QStringConverter::Utf8;
    qsizetype bom = 0;
    encoding = QStringLiteral("UTF-8");
    inferred = false;
    // UTF-32 must precede UTF-16: their little-endian signatures overlap.
    if (bytes.startsWith(QByteArray::fromHex("fffe0000"))) { type = QStringConverter::Utf32LE; bom = 4; encoding = QStringLiteral("UTF-32LE"); }
    else if (bytes.startsWith(QByteArray::fromHex("0000feff"))) { type = QStringConverter::Utf32BE; bom = 4; encoding = QStringLiteral("UTF-32BE"); }
    else if (bytes.startsWith(QByteArray::fromHex("fffe"))) { type = QStringConverter::Utf16LE; bom = 2; encoding = QStringLiteral("UTF-16LE"); }
    else if (bytes.startsWith(QByteArray::fromHex("feff"))) { type = QStringConverter::Utf16BE; bom = 2; encoding = QStringLiteral("UTF-16BE"); }
    else if (bytes.startsWith(QByteArray::fromHex("efbbbf"))) bom = 3;
    // Qt can leave an incomplete final code unit buffered without reporting a
    // decoding error. The entire byte snapshot must contain complete units.
    const auto payloadBytes = bytes.size() - bom;
    if (((type == QStringConverter::Utf16LE || type == QStringConverter::Utf16BE) && payloadBytes % 2 != 0)
        || ((type == QStringConverter::Utf32LE || type == QStringConverter::Utf32BE) && payloadBytes % 4 != 0))
        return false;
    if (type == QStringConverter::Utf32LE || type == QStringConverter::Utf32BE) {
        for (qsizetype offset = bom; offset < bytes.size(); offset += 4) {
            const auto *unit = bytes.constData() + offset;
            const auto scalar = type == QStringConverter::Utf32LE
                ? qFromLittleEndian<quint32>(unit) : qFromBigEndian<quint32>(unit);
            if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
        }
    }
    // Remove exactly the physical signature. ConvertInitialBom preserves an
    // intentional second U+FEFF instead of silently removing it as well.
    QStringDecoder decoder(type, QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
    text = decoder(QByteArrayView(bytes).sliced(bom));
    if (!decoder.hasError()) return true;
    if (bom > 0) return false;
#ifdef Q_OS_WIN
    const int count = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.constData(), static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return false;
    text.resize(count);
    if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.constData(), static_cast<int>(bytes.size()),
                          reinterpret_cast<LPWSTR>(text.data()), count) != count) return false;
    encoding = QStringLiteral("CP932");
    inferred = true;
    return true;
#else
    return false;
#endif
}
bool contentValid(const WorkReferenceRecord &record)
{
    const auto &content = record.content;
    if (content.size() != 1) return false;
    qsizetype total = 0;
    bool anyText = false;
    const auto check = [&](const QString &text) {
        qsizetype count = 0;
        if (!validText(text, &count) || count > WorkReferenceCharacters - total) return false;
        total += count;
        anyText = anyText || !text.trimmed().isEmpty();
        return true;
    };
    if (record.format == QStringLiteral("pdf")) {
        if (!content.value(QStringLiteral("pages")).isArray()) return false;
        const auto pages = content.value(QStringLiteral("pages")).toArray();
        if (pages.size() > WorkReferencePdfPages) return false;
        for (qsizetype index = 0; index < pages.size(); ++index) {
            if (!pages.at(index).isObject()) return false;
            const auto page = pages.at(index).toObject();
            if (page.size() != 2 || !page.value(QStringLiteral("page")).isDouble()
                || page.value(QStringLiteral("page")).toInteger() != index + 1
                || !page.value(QStringLiteral("text")).isString()
                || !check(page.value(QStringLiteral("text")).toString())) return false;
        }
    } else if (subtitleFormat(record.format)) {
        if (!content.value(QStringLiteral("cues")).isArray()) return false;
        const auto cues = content.value(QStringLiteral("cues")).toArray();
        qint64 expanded = 0;
        for (qsizetype index = 0; index < cues.size(); ++index) {
            if (!cues.at(index).isObject()) return false;
            const auto cue = cues.at(index).toObject();
            const auto start = cue.value(QStringLiteral("start_ms"));
            const auto end = cue.value(QStringLiteral("end_ms"));
            const auto id = cue.value(QStringLiteral("id"));
            const auto text = cue.value(QStringLiteral("text"));
            if (cue.size() != 4 || !id.isDouble() || id.toInteger(-1) != index + 1
                || !start.isDouble() || !end.isDouble() || start.toInteger(-1) < 0
                || end.toInteger(-1) < start.toInteger(-1) || !text.isString()
                || !check(text.toString())) return false;
            const auto length = text.toString().size();
            if (length > SubtitleFileLoader::MaxExpandedTextCodeUnits - expanded) return false;
            expanded += length;
        }
        if (cues.isEmpty()) return false;
    } else if (supportedText(record.format)) {
        if (!content.value(QStringLiteral("text")).isString()
            || !check(content.value(QStringLiteral("text")).toString())) return false;
    } else return false;
    return (record.state == QStringLiteral("ready")) == anyText;
}
}

QString workReferenceContentVersion(const WorkReferenceRecord &record)
{
    QJsonObject identity{{QStringLiteral("sourceHash"), record.sourceHash},
        {QStringLiteral("format"), record.format}, {QStringLiteral("encoding"), record.encoding},
        {QStringLiteral("encodingInferred"), record.encodingInferred},
        {QStringLiteral("extractorVersion"), record.extractorVersion}, {QStringLiteral("content"), record.content}};
    return sha256(QJsonDocument(identity).toJson(QJsonDocument::Compact));
}

bool validWorkReferenceCache(const WorkReferenceRecord &record)
{
    if (record.missing || (record.state != QStringLiteral("ready") && record.state != QStringLiteral("no-text"))
        || record.extractorVersion != QLatin1StringView(WorkReferenceExtractorVersion)
        || !hashValid(record.sourceHash) || !hashValid(record.contentVersion)
        || record.contentVersion != workReferenceContentVersion(record)) return false;
    static const QSet<QString> encodings{QStringLiteral("UTF-8"), QStringLiteral("UTF-16LE"),
        QStringLiteral("UTF-16BE"), QStringLiteral("UTF-32LE"), QStringLiteral("UTF-32BE"), QStringLiteral("CP932")};
    if (record.format == QStringLiteral("pdf")) {
        if (record.encoding != QStringLiteral("PDF") || record.encodingInferred) return false;
    } else if (!encodings.contains(record.encoding)
        || record.encodingInferred != (record.encoding == QStringLiteral("CP932"))) return false;
    return contentValid(record);
}

WorkReferenceRecord extractWorkReference(const WorkReferenceCandidate &candidate,
    const std::atomic_bool &cancelled, const WorkReferenceRecord *previous, bool force)
{
    WorkReferenceRecord result;
    result.relativePath = candidate.relativePath;
    result.format = candidate.format;
    result.source = candidate.source;
    result.audioRelativePath = candidate.audioRelativePath;
    result.completedAt = candidate.completedAt;
    result.warning = candidate.warning;
    result.extractorVersion = QString::fromLatin1(WorkReferenceExtractorVersion);
    result.state = QStringLiteral("failed");
    const auto fail = [&](const QString &message) {
        result.error = message;
        result.content = {};
        result.contentVersion.clear();
        return result;
    };
    const auto stop = [&] {
        if (!cancelled.load()) return false;
        result.state = QStringLiteral("cancelled");
        result.error = QStringLiteral("参考资料提取已取消。");
        result.content = {};
        result.contentVersion.clear();
        return true;
    };
    if (stop()) return result;
    const bool pdf = candidate.format == QStringLiteral("pdf");
    const bool subtitle = subtitleFormat(candidate.format);
    if (!pdf && !subtitle && !supportedText(candidate.format)) return fail(QStringLiteral("不支持的参考资料格式。"));
    const auto maximumBytes = pdf ? WorkReferencePdfBytes : WorkReferenceTextBytes;
    const QFileInfo before(candidate.absolutePath);
    if (!safePath(candidate.absolutePath) || before.size() < 0 || before.size() > maximumBytes)
        return fail(QStringLiteral("资料不是安全的普通文件，或超过大小限制。"));
    QFile file(candidate.absolutePath);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential()) return fail(QStringLiteral("无法读取参考资料文件。"));
    QByteArray bytes;
    while (!file.atEnd()) {
        if (stop()) return result;
        const auto part = file.read(std::min<qint64>(65536, maximumBytes + 1 - bytes.size()));
        if (file.error() != QFileDevice::NoError || (part.isEmpty() && !file.atEnd()))
            return fail(QStringLiteral("读取参考资料失败。"));
        bytes += part;
        if (bytes.size() > maximumBytes) return fail(QStringLiteral("参考资料超过大小限制。"));
    }
    const QFileInfo after(candidate.absolutePath);
    if (!safePath(candidate.absolutePath) || before.size() != bytes.size()
        || before.size() != after.size() || before.lastModified() != after.lastModified())
        return fail(QStringLiteral("读取时参考资料发生变化，请更新资料后重试。"));
    result.sourceHash = sha256(bytes);
    if (stop()) return result;
    if (!force && previous && previous->sourceHash == result.sourceHash && previous->format == result.format
        && validWorkReferenceCache(*previous)) {
        auto reused = *previous;
        reused.relativePath = result.relativePath;
        reused.source = result.source;
        reused.audioRelativePath = result.audioRelativePath;
        reused.completedAt = result.completedAt;
        reused.warning = result.warning;
        reused.error.clear();
        reused.cacheReused = true;
        return reused;
    }
    bool anyText = false;
    if (pdf) {
        result.encoding = QStringLiteral("PDF");
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QPdfDocument document;
        document.load(&buffer);
        if (document.error() == QPdfDocument::Error::IncorrectPassword)
            return fail(QStringLiteral("PDF 需要密码，参考资料提取暂不解锁。"));
        if (document.status() != QPdfDocument::Status::Ready || document.error() != QPdfDocument::Error::None)
            return fail(QStringLiteral("无法解析 PDF，文件可能损坏或受保护。"));
        if (document.pageCount() > WorkReferencePdfPages) return fail(QStringLiteral("PDF 超过 200 页限制。"));
        qsizetype total = 0;
        QJsonArray pages;
        for (int index = 0; index < document.pageCount(); ++index) {
            if (stop()) return result;
            auto text = document.getAllText(index).text();
            normalize(text);
            qsizetype count = 0;
            if (!validText(text, &count)) return fail(QStringLiteral("PDF 提取文字含无效 Unicode 或异常控制字符。"));
            if (count > WorkReferenceCharacters - total) return fail(QStringLiteral("参考资料正文超过 100 万字符限制。"));
            total += count;
            anyText = anyText || !text.trimmed().isEmpty();
            pages.push_back(QJsonObject{{QStringLiteral("page"), index + 1}, {QStringLiteral("text"), text}});
        }
        result.content = {{QStringLiteral("pages"), pages}};
    } else {
        QString text;
        if (!decode(bytes, text, result.encoding, result.encodingInferred))
            return fail(QStringLiteral("参考资料编码无效，自动读取失败。"));
        normalize(text);
        qsizetype count = 0;
        if (!validText(text, &count)) return fail(QStringLiteral("参考资料含无效 Unicode、NUL 或异常控制字符。"));
        if (count > WorkReferenceCharacters) return fail(QStringLiteral("参考资料正文超过 100 万字符限制。"));
        if (!subtitle) {
            anyText = !text.trimmed().isEmpty();
            result.content = {{QStringLiteral("text"), text}};
        } else {
            QList<SubtitleEntry> entries;
            bool hasErrors = false;
            QStringList diagnostics;
            const auto collect = [&](const auto &parsed) {
                entries = parsed.entries;
                hasErrors = parsed.hasErrors();
                for (const auto &diagnostic : parsed.diagnostics)
                    if (diagnostics.size() < 5) diagnostics.push_back(QStringLiteral("第 %1 行：%2").arg(diagnostic.line).arg(diagnostic.message));
            };
            if (candidate.format == QStringLiteral("srt")) collect(SrtParser::parse(text));
            else if (candidate.format == QStringLiteral("vtt")) collect(VttParser::parse(text));
            else collect(LrcParser::parse(text));
            if (hasErrors || entries.isEmpty())
                return fail(QStringLiteral("字幕解析未完整成功：%1").arg(diagnostics.join(QLatin1Char('\n'))));
            qint64 expanded = 0;
            qsizetype characters = 0;
            QJsonArray cues;
            for (const auto &entry : entries) {
                if (stop()) return result;
                qsizetype cueCount = 0;
                if (!validText(entry.text, &cueCount)) return fail(QStringLiteral("字幕正文含无效字符。"));
                if (entry.text.size() > SubtitleFileLoader::MaxExpandedTextCodeUnits - expanded
                    || cueCount > WorkReferenceCharacters - characters)
                    return fail(QStringLiteral("展开后的字幕正文超过资料限制。"));
                expanded += entry.text.size();
                characters += cueCount;
                anyText = anyText || !entry.text.trimmed().isEmpty();
                cues.push_back(QJsonObject{{QStringLiteral("id"), cues.size() + 1},
                    {QStringLiteral("start_ms"), entry.startMs}, {QStringLiteral("end_ms"), entry.endMs},
                    {QStringLiteral("text"), entry.text}});
            }
            result.content = {{QStringLiteral("cues"), cues}};
        }
    }
    if (stop()) return result;
    result.state = anyText ? QStringLiteral("ready") : QStringLiteral("no-text");
    result.contentVersion = workReferenceContentVersion(result);
    return result;
}
} // namespace asmr
