#include "subtitle/LrcParser.h"

#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <limits>
#include <optional>

namespace asmr {
namespace {

struct TimestampResult
{
    std::optional<qint64> value;
    LrcDiagnosticCode error = LrcDiagnosticCode::InvalidTimestamp;
};

struct PendingEntry
{
    qint64 startMs;
    QString text;
    qsizetype line;
};

bool isAsciiDigit(QChar character)
{
    return character.unicode() >= u'0' && character.unicode() <= u'9';
}

bool allAsciiDigits(QStringView text)
{
    return !text.isEmpty()
           && std::all_of(text.begin(), text.end(), isAsciiDigit);
}

bool isHorizontalSpace(QChar character)
{
    return character == QChar(' ') || character == QChar('\t');
}

TimestampResult parseTimestamp(QStringView timestamp)
{
    TimestampResult result;
    const qsizetype colon = timestamp.indexOf(QChar(':'));
    const qsizetype dot = timestamp.indexOf(QChar('.'));
    if (colon < 2 || dot != colon + 3
        || timestamp.indexOf(QChar(':'), colon + 1) >= 0
        || timestamp.indexOf(QChar('.'), dot + 1) >= 0) {
        return result;
    }

    const QStringView minutesText = timestamp.left(colon);
    const QStringView secondsText = timestamp.mid(colon + 1, 2);
    const QStringView fractionText = timestamp.mid(dot + 1);
    if (!allAsciiDigits(minutesText) || !allAsciiDigits(secondsText)
        || (fractionText.size() != 2 && fractionText.size() != 3)
        || !allAsciiDigits(fractionText)) {
        return result;
    }

    const qint64 seconds = (secondsText.at(0).unicode() - u'0') * 10
                           + secondsText.at(1).unicode() - u'0';
    if (seconds > 59)
        return result;

    qint64 fraction = 0;
    for (const QChar character : fractionText)
        fraction = fraction * 10 + character.unicode() - u'0';
    if (fractionText.size() == 2)
        fraction *= 10;
    const qint64 remainder = seconds * 1000 + fraction;
    constexpr qint64 maximum = std::numeric_limits<qint64>::max();
    constexpr qint64 millisecondsPerMinute = 60000;
    const qint64 maximumMinutes = (maximum - remainder) / millisecondsPerMinute;

    qint64 minutes = 0;
    for (const QChar character : minutesText) {
        const qint64 digit = character.unicode() - u'0';
        if (minutes > maximumMinutes / 10
            || (minutes == maximumMinutes / 10 && digit > maximumMinutes % 10)) {
            result.error = LrcDiagnosticCode::TimestampOverflow;
            return result;
        }
        minutes = minutes * 10 + digit;
    }
    result.value = minutes * millisecondsPerMinute + remainder;
    return result;
}

bool isTimestampShaped(QStringView tag)
{
    if (tag.isEmpty())
        return false;
    return tag.front().isDigit() || tag.front() == QChar('+')
           || tag.front() == QChar('-');
}

bool isKnownMetadata(QStringView tag)
{
    const qsizetype colon = tag.indexOf(QChar(':'));
    if (colon < 0)
        return false;
    const QString name = tag.left(colon).toString().toLower();
    return name == QStringLiteral("ti") || name == QStringLiteral("ar")
           || name == QStringLiteral("al") || name == QStringLiteral("by")
           || name == QStringLiteral("au") || name == QStringLiteral("re")
           || name == QStringLiteral("ve") || name == QStringLiteral("length");
}

bool hasEnhancedTimestamp(QStringView line)
{
    for (qsizetype opening = line.indexOf(QChar('<')); opening >= 0;
         opening = line.indexOf(QChar('<'), opening + 1)) {
        // Match from each '<' rather than consuming through the next '>'. A
        // literal '<' before a word tag must not hide that later tag. Scanning
        // only contiguous numeric fields also avoids repeatedly searching the
        // rest of a malformed long line for a closing bracket.
        qsizetype cursor = opening + 1;
        const qsizetype minuteStart = cursor;
        while (cursor < line.size() && isAsciiDigit(line.at(cursor)))
            ++cursor;
        if (cursor == minuteStart || cursor >= line.size() || line.at(cursor) != QChar(':'))
            continue;
        const qsizetype secondStart = ++cursor;
        while (cursor < line.size() && isAsciiDigit(line.at(cursor)))
            ++cursor;
        if (cursor == secondStart)
            continue;
        if (cursor < line.size() && line.at(cursor) == QChar('.')) {
            const qsizetype fractionStart = ++cursor;
            while (cursor < line.size() && isAsciiDigit(line.at(cursor)))
                ++cursor;
            if (cursor == fractionStart)
                continue;
        }
        if (cursor < line.size() && line.at(cursor) == QChar('>'))
            return true;
    }
    return false;
}

void addDiagnostic(LrcParseResult &result, LrcDiagnosticSeverity severity,
                   LrcDiagnosticCode code, qsizetype line, const QString &message)
{
    result.diagnostics.append(LrcDiagnostic{severity, code, line, message});
}

} // namespace

bool LrcParseResult::hasErrors() const
{
    return std::any_of(diagnostics.cbegin(), diagnostics.cend(),
                       [](const LrcDiagnostic &diagnostic) {
                           return diagnostic.severity == LrcDiagnosticSeverity::Error;
                       });
}

LrcParseResult LrcParser::parse(const QString &text, const LrcParseOptions &options)
{
    LrcParseResult result;
    if (options.lastEntryDurationMs <= 0 || options.maxEntryDurationMs <= 0) {
        addDiagnostic(result, LrcDiagnosticSeverity::Error,
                      LrcDiagnosticCode::InvalidOptions, 1,
                      QStringLiteral("LRC display durations must both be positive."));
        return result;
    }

    QString normalized = text;
    if (normalized.startsWith(QChar(0xFEFF)))
        normalized.remove(0, 1);
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar('\r'), QChar('\n'));
    if (normalized.trimmed().isEmpty()) {
        addDiagnostic(result, LrcDiagnosticSeverity::Warning,
                      LrcDiagnosticCode::EmptyInput, 1,
                      QStringLiteral("LRC input contains no text."));
        return result;
    }

    QList<PendingEntry> pendingEntries;
    QList<qint64> timeline;
    const QStringList lines = normalized.split(QChar('\n'));
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const QStringView line(lines.at(index));
        const qsizetype sourceLine = index + 1;
        if (line.trimmed().isEmpty())
            continue;
        if (hasEnhancedTimestamp(line)) {
            addDiagnostic(result, LrcDiagnosticSeverity::Error,
                          LrcDiagnosticCode::UnsupportedEnhancedLrc, sourceLine,
                          QStringLiteral("Enhanced LRC word timestamps are unsupported; "
                                         "this line was skipped."));
            continue;
        }

        qsizetype cursor = 0;
        while (cursor < line.size() && isHorizontalSpace(line.at(cursor)))
            ++cursor;
        QList<qint64> lineTimes;
        bool hasTagError = false;
        bool unclosedTag = false;
        while (cursor < line.size() && line.at(cursor) == QChar('[')) {
            const qsizetype closing = line.indexOf(QChar(']'), cursor + 1);
            if (closing < 0) {
                addDiagnostic(result, LrcDiagnosticSeverity::Error,
                              LrcDiagnosticCode::MalformedTag, sourceLine,
                              QStringLiteral("Unclosed LRC prefix tag; this line was skipped."));
                unclosedTag = true;
                break;
            }
            const QStringView tag = line.mid(cursor + 1, closing - cursor - 1);
            if (tag.isEmpty() || tag.contains(QChar('['))) {
                addDiagnostic(result, LrcDiagnosticSeverity::Error,
                              LrcDiagnosticCode::MalformedTag, sourceLine,
                              QStringLiteral("LRC prefix tag is empty or contains a nested '['."));
                hasTagError = true;
            } else if (isTimestampShaped(tag)) {
                const TimestampResult timestamp = parseTimestamp(tag);
                if (timestamp.value) {
                    lineTimes.append(*timestamp.value);
                } else {
                    addDiagnostic(result, LrcDiagnosticSeverity::Error,
                                  timestamp.error, sourceLine,
                                  timestamp.error == LrcDiagnosticCode::TimestampOverflow
                                      ? QStringLiteral("LRC timestamp exceeds qint64 milliseconds.")
                                      : QStringLiteral("Invalid LRC timestamp; expected "
                                                       "mm+:ss.xx or mm+:ss.xxx "
                                                       "with seconds 00..59."));
                    hasTagError = true;
                }
            } else if (!isKnownMetadata(tag)) {
                addDiagnostic(result, LrcDiagnosticSeverity::Warning,
                              LrcDiagnosticCode::UnsupportedTag, sourceLine,
                              QStringLiteral("Unsupported LRC tag was ignored; "
                                             "offsets and other extensions are not applied."));
            }
            cursor = closing + 1;
        }
        if (unclosedTag)
            continue;

        const QStringView lyric = line.mid(cursor);
        if (lineTimes.isEmpty()) {
            if (!lyric.trimmed().isEmpty() && !hasTagError) {
                addDiagnostic(result, LrcDiagnosticSeverity::Error,
                              LrcDiagnosticCode::MissingTimestamp, sourceLine,
                              QStringLiteral("Nonblank LRC lyric has no valid prefix timestamp."));
            }
            continue;
        }

        timeline.append(lineTimes);
        if (lyric.trimmed().isEmpty()) {
            addDiagnostic(result, LrcDiagnosticSeverity::Warning,
                          LrcDiagnosticCode::EmptyTextMarker, sourceLine,
                          QStringLiteral("Empty LRC lyric is a clear marker: "
                                         "it limits earlier lyrics and is not displayed."));
            continue;
        }
        const QString lyricText = lyric.toString();
        for (const qint64 start : lineTimes)
            pendingEntries.append(PendingEntry{start, lyricText, sourceLine});
    }

    std::sort(timeline.begin(), timeline.end());
    timeline.erase(std::unique(timeline.begin(), timeline.end()), timeline.end());
    constexpr qint64 maximum = std::numeric_limits<qint64>::max();
    for (const PendingEntry &pending : pendingEntries) {
        if (pending.startMs == maximum) {
            addDiagnostic(result, LrcDiagnosticSeverity::Error,
                          LrcDiagnosticCode::EndTimeOverflow, pending.line,
                          QStringLiteral("LRC timestamp at qint64 maximum cannot form "
                                         "a positive display interval."));
            continue;
        }

        const auto next = std::upper_bound(timeline.cbegin(), timeline.cend(),
                                           pending.startMs);
        qint64 end = maximum;
        if (next != timeline.cend() && *next - pending.startMs <= options.maxEntryDurationMs) {
            end = *next;
        } else {
            const qint64 duration = next == timeline.cend()
                                       ? std::min(options.lastEntryDurationMs,
                                                  options.maxEntryDurationMs)
                                       : options.maxEntryDurationMs;
            if (duration > maximum - pending.startMs) {
                addDiagnostic(result, LrcDiagnosticSeverity::Warning,
                              LrcDiagnosticCode::EndTimeClamped, pending.line,
                              QStringLiteral("LRC display end exceeds qint64 milliseconds "
                                             "and was clamped to its maximum."));
            } else {
                end = pending.startMs + duration;
            }
        }
        result.entries.append(SubtitleEntry{pending.startMs, end, pending.text});
    }
    return result;
}

} // namespace asmr
