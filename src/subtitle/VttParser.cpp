#include "subtitle/VttParser.h"

#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <limits>
#include <optional>

namespace asmr {
namespace {

struct TimeRange
{
    qint64 startMs;
    qint64 endMs;
};

struct TimingResult
{
    std::optional<TimeRange> range;
    VttDiagnosticCode error = VttDiagnosticCode::InvalidTimingLine;
    bool hasSettings = false;
};

bool isAsciiDigit(QChar character)
{
    return character.unicode() >= u'0' && character.unicode() <= u'9';
}

bool isHorizontalSpace(QChar character)
{
    return character == QChar(' ') || character == QChar('\t');
}

QStringView trimHorizontalSpace(QStringView text)
{
    while (!text.isEmpty() && isHorizontalSpace(text.front()))
        text = text.mid(1);
    while (!text.isEmpty() && isHorizontalSpace(text.back()))
        text = text.left(text.size() - 1);
    return text;
}

std::optional<qint64> parseUnsigned(QStringView digits)
{
    if (digits.isEmpty())
        return std::nullopt;
    qint64 value = 0;
    constexpr qint64 maximum = std::numeric_limits<qint64>::max();
    for (const QChar character : digits) {
        if (!isAsciiDigit(character))
            return std::nullopt;
        const int digit = character.unicode() - u'0';
        if (value > (maximum - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

std::optional<qint64> parseTimestamp(QStringView timestamp)
{
    if (timestamp.size() < 9)
        return std::nullopt;
    const qsizetype dot = timestamp.size() - 4;
    if (timestamp.at(dot) != QChar('.'))
        return std::nullopt;
    const qsizetype firstColon = timestamp.indexOf(QChar(':'));
    if (firstColon < 0)
        return std::nullopt;
    const qsizetype secondColon = timestamp.indexOf(QChar(':'), firstColon + 1);
    qint64 hours = 0;
    qsizetype minuteOffset = 0;
    qsizetype secondOffset = firstColon + 1;
    if (secondColon < 0) {
        if (firstColon != 2 || dot != 5)
            return std::nullopt;
    } else {
        if (firstColon < 2 || secondColon != firstColon + 3
            || dot != secondColon + 3
            || timestamp.indexOf(QChar(':'), secondColon + 1) >= 0) {
            return std::nullopt;
        }
        const auto parsedHours = parseUnsigned(timestamp.left(firstColon));
        if (!parsedHours)
            return std::nullopt;
        hours = *parsedHours;
        minuteOffset = firstColon + 1;
        secondOffset = secondColon + 1;
    }
    const auto minutes = parseUnsigned(timestamp.mid(minuteOffset, 2));
    const auto seconds = parseUnsigned(timestamp.mid(secondOffset, 2));
    const auto milliseconds = parseUnsigned(timestamp.mid(dot + 1, 3));
    if (!minutes || !seconds || !milliseconds || *minutes > 59 || *seconds > 59)
        return std::nullopt;

    const qint64 remainder = (*minutes * 60 + *seconds) * 1000 + *milliseconds;
    constexpr qint64 millisecondsPerHour = 3600000;
    constexpr qint64 maximum = std::numeric_limits<qint64>::max();
    if (hours > (maximum - remainder) / millisecondsPerHour)
        return std::nullopt;
    return hours * millisecondsPerHour + remainder;
}

TimingResult parseTimingLine(const QString &line)
{
    TimingResult result;
    const QStringView view = trimHorizontalSpace(QStringView(line));
    const qsizetype arrow = view.indexOf(QStringView(u"-->"));
    if (arrow <= 0 || arrow + 3 >= view.size()
        || view.indexOf(QStringView(u"-->"), arrow + 3) >= 0
        || !isHorizontalSpace(view.at(arrow - 1))
        || !isHorizontalSpace(view.at(arrow + 3))) {
        return result;
    }
    const QStringView startText = trimHorizontalSpace(view.left(arrow));
    const QStringView tail = trimHorizontalSpace(view.mid(arrow + 3));
    qsizetype endLength = 0;
    while (endLength < tail.size() && !isHorizontalSpace(tail.at(endLength)))
        ++endLength;
    const auto start = parseTimestamp(startText);
    const auto end = parseTimestamp(tail.left(endLength));
    if (!start || !end) {
        result.error = VttDiagnosticCode::InvalidTimestamp;
        return result;
    }
    result.range = TimeRange{*start, *end};
    result.hasSettings = !trimHorizontalSpace(tail.mid(endLength)).isEmpty();
    return result;
}

bool hasTimestampShape(QStringView text)
{
    text = trimHorizontalSpace(text);
    if (text.isEmpty())
        return false;
    return (isAsciiDigit(text.front()) || text.front() == QChar('-')
            || text.front() == QChar('+')) && text.contains(QChar(':'));
}

bool isTimingShapedLine(const QString &line)
{
    const qsizetype arrow = line.indexOf(QStringLiteral("-->"));
    if (arrow < 0)
        return false;
    const QStringView view(line);
    return hasTimestampShape(view.left(arrow))
           && hasTimestampShape(view.mid(arrow + 3));
}

bool isNote(const QString &line)
{
    return line == QStringLiteral("NOTE")
           || (line.startsWith(QStringLiteral("NOTE")) && line.size() > 4
               && isHorizontalSpace(line.at(4)));
}

bool hasLiteralMarkup(const QString &line)
{
    if (line.contains(QChar('<')))
        return true;
    const qsizetype ampersand = line.indexOf(QChar('&'));
    return ampersand >= 0 && line.indexOf(QChar(';'), ampersand + 2) >= 0;
}

void addDiagnostic(VttParseResult &result, VttDiagnosticSeverity severity,
                   VttDiagnosticCode code, qsizetype line, const QString &message)
{
    result.diagnostics.append(VttDiagnostic{severity, code, line, message});
}

void skipBlock(const QStringList &lines, qsizetype &index)
{
    while (index < lines.size() && !lines.at(index).trimmed().isEmpty())
        ++index;
}

void reportMissingBlankLine(VttParseResult &result, qsizetype line)
{
    addDiagnostic(result, VttDiagnosticSeverity::Warning,
                  VttDiagnosticCode::MissingBlankLine, line,
                  QStringLiteral("Missing blank line before the next cue; "
                                 "a timing-shaped line is treated as a new cue."));
}

} // namespace

bool VttParseResult::hasErrors() const
{
    return std::any_of(diagnostics.cbegin(), diagnostics.cend(),
                       [](const VttDiagnostic &diagnostic) {
        return diagnostic.severity == VttDiagnosticSeverity::Error;
    });
}

VttParseResult VttParser::parse(const QString &text)
{
    VttParseResult result;
    QString normalized = text;
    if (normalized.startsWith(QChar(u'\ufeff')))
        normalized.remove(0, 1);
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar('\r'), QChar('\n'));
    if (normalized.trimmed().isEmpty()) {
        addDiagnostic(result, VttDiagnosticSeverity::Warning,
                      VttDiagnosticCode::EmptyInput, 1,
                      QStringLiteral("Input contains no WebVTT subtitle entries."));
        return result;
    }

    const QStringList lines = normalized.split(QChar('\n'));
    const QString &header = lines.front();
    if (!header.startsWith(QStringLiteral("WEBVTT"), Qt::CaseInsensitive)
        || (header.size() > 6 && !isHorizontalSpace(header.at(6)))
        || header.contains(QStringLiteral("-->"))) {
        addDiagnostic(result, VttDiagnosticSeverity::Error,
                      VttDiagnosticCode::InvalidHeader, 1,
                      QStringLiteral("The first line must be WEBVTT, optionally followed "
                                     "by a space/tab and comment, without '-->'."));
        return result;
    }

    qsizetype index = 1;
    while (index < lines.size() && !lines.at(index).trimmed().isEmpty()) {
        // Recover a cue even if the mandatory header separator is missing.
        if (isTimingShapedLine(lines.at(index))
            || (index + 1 < lines.size() && isTimingShapedLine(lines.at(index + 1)))) {
            addDiagnostic(result, VttDiagnosticSeverity::Warning,
                          VttDiagnosticCode::MissingHeaderSeparator, index + 1,
                          QStringLiteral("Missing blank line after the WEBVTT header; "
                                         "parsing resumes at the first cue."));
            break;
        }
        addDiagnostic(result, VttDiagnosticSeverity::Warning,
                      VttDiagnosticCode::UnsupportedHeaderMetadata, index + 1,
                      QStringLiteral("Unsupported WebVTT header metadata is ignored."));
        ++index;
    }

    while (index < lines.size()) {
        if (lines.at(index).trimmed().isEmpty()) {
            ++index;
            continue;
        }
        if (isNote(lines.at(index))) {
            skipBlock(lines, index);
            continue;
        }

        QStringView directive(lines.at(index));
        while (!directive.isEmpty() && isHorizontalSpace(directive.back()))
            directive = directive.left(directive.size() - 1);
        const bool followingTiming = index + 1 < lines.size()
                                     && isTimingShapedLine(lines.at(index + 1));
        if (!followingTiming
            && (directive == QStringView(u"STYLE") || directive == QStringView(u"REGION"))) {
            const bool style = directive == QStringView(u"STYLE");
            addDiagnostic(result, VttDiagnosticSeverity::Warning,
                          style ? VttDiagnosticCode::IgnoredStyleBlock
                                : VttDiagnosticCode::IgnoredRegionBlock,
                          index + 1,
                          style ? QStringLiteral("WebVTT STYLE blocks are ignored; subtitles use plain text.")
                                : QStringLiteral("WebVTT REGION blocks are ignored; subtitles use the player display area."));
            skipBlock(lines, index);
            continue;
        }

        if (!lines.at(index).contains(QStringLiteral("-->"))) {
            const qsizetype identifierLine = index + 1;
            ++index; // Optional cue identifier, intentionally not stored.
            if (index >= lines.size() || lines.at(index).trimmed().isEmpty()) {
                addDiagnostic(result, VttDiagnosticSeverity::Error,
                              VttDiagnosticCode::MissingTimingLine, identifierLine,
                              QStringLiteral("Cue identifier is not followed by a timing line."));
                continue;
            }
        }

        const qsizetype timingLine = index + 1;
        const TimingResult timing = parseTimingLine(lines.at(index));
        if (!timing.range) {
            addDiagnostic(result, VttDiagnosticSeverity::Error, timing.error, timingLine,
                          timing.error == VttDiagnosticCode::InvalidTimestamp
                          ? QStringLiteral("Cue times must use MM:SS.mmm or HH+:MM:SS.mmm with ASCII digits, "
                                           "minutes/seconds 00..59 and a value fitting qint64 milliseconds.")
                          : QStringLiteral("Expected one '-->' separator with space/tab on both sides."));
            ++index;
            while (index < lines.size() && !lines.at(index).trimmed().isEmpty()) {
                if (isTimingShapedLine(lines.at(index))) {
                    reportMissingBlankLine(result, index + 1);
                    break;
                }
                ++index;
            }
            continue;
        }
        if (timing.hasSettings) {
            addDiagnostic(result, VttDiagnosticSeverity::Warning,
                          VttDiagnosticCode::IgnoredCueSettings, timingLine,
                          QStringLiteral("Cue settings are ignored; subtitles use the player display area."));
        }
        ++index;

        QStringList textLines;
        qsizetype markupLine = 0;
        while (index < lines.size() && !lines.at(index).trimmed().isEmpty()) {
            if (isTimingShapedLine(lines.at(index))) {
                reportMissingBlankLine(result, index + 1);
                break;
            }
            if (markupLine == 0 && hasLiteralMarkup(lines.at(index)))
                markupLine = index + 1;
            textLines.append(lines.at(index));
            ++index;
        }
        const TimeRange range = *timing.range;
        if (range.endMs < range.startMs) {
            addDiagnostic(result, VttDiagnosticSeverity::Error,
                          VttDiagnosticCode::EndBeforeStart, timingLine,
                          QStringLiteral("Cue end time is earlier than its start time."));
        }
        if (textLines.isEmpty()) {
            addDiagnostic(result, VttDiagnosticSeverity::Error,
                          VttDiagnosticCode::EmptyText, timingLine,
                          QStringLiteral("Cue contains no text."));
        }
        if (range.endMs < range.startMs || textLines.isEmpty())
            continue;
        if (range.endMs == range.startMs) {
            addDiagnostic(result, VttDiagnosticSeverity::Warning,
                          VttDiagnosticCode::ZeroDuration, timingLine,
                          QStringLiteral("Cue start and end times are equal; the empty interval is retained."));
        }
        if (markupLine != 0) {
            addDiagnostic(result, VttDiagnosticSeverity::Warning,
                          VttDiagnosticCode::LiteralInlineMarkup, markupLine,
                          QStringLiteral("Inline markup, entities and inline timestamps are shown literally; "
                                         "they are not interpreted or rendered as rich text."));
        }
        result.entries.append(SubtitleEntry{range.startMs, range.endMs,
                                            textLines.join(QChar('\n'))});
    }
    return result;
}

} // namespace asmr
