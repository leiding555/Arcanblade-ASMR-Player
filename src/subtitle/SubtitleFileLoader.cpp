#include "subtitle/SubtitleFileLoader.h"

#include "subtitle/LrcParser.h"
#include "subtitle/SrtParser.h"
#include "subtitle/VttParser.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <algorithm>
#include <utility>

namespace asmr {
namespace {

SubtitleFileLoadResult failure(SubtitleFileError error, const QString &message)
{
    SubtitleFileLoadResult result;
    result.error = error;
    result.errorMessage = message;
    return result;
}

SubtitleFileLoadResult tooLarge()
{
    return failure(SubtitleFileError::TooLarge,
                   QStringLiteral("字幕文件超过 4 MiB 大小上限。"));
}

} // namespace

bool SubtitleFileLoadResult::succeeded() const
{
    return error == SubtitleFileError::None && !entries.isEmpty();
}

SubtitleFileLoadResult SubtitleFileLoader::load(const QString &path, SubtitleFormat format)
{
    if (format != SubtitleFormat::Srt && format != SubtitleFormat::Vtt
        && format != SubtitleFormat::Lrc) {
        return failure(SubtitleFileError::UnsupportedFormat,
                       QStringLiteral("不支持此字幕格式，请明确选择 SRT、VTT 或 LRC。"));
    }

    // Reject directories and special files before opening, including pipes whose
    // open/read could otherwise wait indefinitely.
    if (!QFileInfo(path).isFile()) {
        return failure(SubtitleFileError::OpenFailed,
                       QStringLiteral("无法打开字幕文件：路径不存在或不是普通文件。\n%1")
                           .arg(path));
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(SubtitleFileError::OpenFailed,
                       QStringLiteral("无法打开字幕文件：%1").arg(file.errorString()));
    }
    if (file.isSequential()) {
        return failure(SubtitleFileError::OpenFailed,
                       QStringLiteral("字幕文件必须是可读取的普通本地文件。"));
    }
    if (file.size() > MaxFileSizeBytes)
        return tooLarge();

    QByteArray bytes;
    constexpr qint64 ReadChunkSize = 64 * 1024;
    while (true) {
        // Read at most one byte beyond the limit, even if the file grows after
        // size() was checked. Never allocate from an untrusted file size.
        const qint64 remaining = MaxFileSizeBytes + 1 - bytes.size();
        const QByteArray chunk = file.read(std::min(ReadChunkSize, remaining));
        if (file.error() != QFileDevice::NoError) {
            return failure(SubtitleFileError::ReadFailed,
                           QStringLiteral("读取字幕文件失败：%1").arg(file.errorString()));
        }
        if (chunk.isEmpty()) {
            if (!file.atEnd()) {
                return failure(SubtitleFileError::ReadFailed,
                               QStringLiteral("读取字幕文件失败：尚未到达文件末尾，但未读到数据。"));
            }
            break;
        }
        bytes.append(chunk);
        if (bytes.size() > MaxFileSizeBytes)
            return tooLarge();
    }

    // Stateless treats a truncated final sequence as invalid rather than
    // buffering it for another chunk. Decode the complete bounded byte stream.
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder(bytes);
    if (decoder.hasError()) {
        return failure(SubtitleFileError::InvalidUtf8,
                       QStringLiteral("字幕文件包含无效的 UTF-8 字节或不完整的字符序列。"
                                      "本阶段仅支持 UTF-8（可带 BOM），请转换编码后重试。"));
    }

    SubtitleFileLoadResult result;
    if (format == SubtitleFormat::Srt) {
        auto parsed = SrtParser::parse(text);
        result.entries = std::move(parsed.entries);
        for (const auto &diagnostic : parsed.diagnostics) {
            result.diagnostics.append(SubtitleLoadDiagnostic{
                diagnostic.severity == SrtDiagnosticSeverity::Error
                    ? SubtitleLoadDiagnosticSeverity::Error
                    : SubtitleLoadDiagnosticSeverity::Warning,
                diagnostic.line, diagnostic.message});
        }
    } else if (format == SubtitleFormat::Vtt) {
        auto parsed = VttParser::parse(text);
        result.entries = std::move(parsed.entries);
        for (const auto &diagnostic : parsed.diagnostics) {
            result.diagnostics.append(SubtitleLoadDiagnostic{
                diagnostic.severity == VttDiagnosticSeverity::Error
                    ? SubtitleLoadDiagnosticSeverity::Error
                    : SubtitleLoadDiagnosticSeverity::Warning,
                diagnostic.line, diagnostic.message});
        }
    } else {
        auto parsed = LrcParser::parse(text);
        result.entries = std::move(parsed.entries);
        for (const auto &diagnostic : parsed.diagnostics) {
            result.diagnostics.append(SubtitleLoadDiagnostic{
                diagnostic.severity == LrcDiagnosticSeverity::Error
                    ? SubtitleLoadDiagnosticSeverity::Error
                    : SubtitleLoadDiagnosticSeverity::Warning,
                diagnostic.line, diagnostic.message});
        }
    }
    // A small LRC can repeat a long lyric for many timestamps. QString shares
    // those strings during parsing, but GUI joining would expand every copy.
    // Check the total conservatively across all entries, without constructing
    // that potentially huge output or overflowing the counter.
    qint64 expandedTextCodeUnits = 0;
    for (const auto &entry : result.entries) {
        if (entry.text.size() > MaxExpandedTextCodeUnits - expandedTextCodeUnits) {
            result.error = SubtitleFileError::ExpandedTextTooLarge;
            result.errorMessage = QStringLiteral("展开后的字幕正文总量超过显示资源上限，"
                                                 "请减少重复时间标签或拆分字幕文件。");
            result.entries.clear();
            return result;
        }
        expandedTextCodeUnits += entry.text.size();
    }

    if (result.entries.isEmpty()) {
        result.error = SubtitleFileError::NoValidEntries;
        QString formatName;
        switch (format) {
        case SubtitleFormat::Srt: formatName = QStringLiteral("SRT"); break;
        case SubtitleFormat::Vtt: formatName = QStringLiteral("VTT"); break;
        case SubtitleFormat::Lrc: formatName = QStringLiteral("LRC"); break;
        }
        result.errorMessage = QStringLiteral("字幕文件中没有可用的 %1 字幕条目，请检查解析诊断。")
                                  .arg(formatName);
    }
    return result;
}

} // namespace asmr
