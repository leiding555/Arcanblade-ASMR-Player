#include "subtitle/SrtParser.h"

#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <array>
#include <optional>

namespace asmr {
namespace {

struct TimeRange
{
    qint64 startMs;
    qint64 endMs;
};

bool isAsciiDigit(QChar character)
{
    return character.unicode() >= u'0' && character.unicode() <= u'9';
}

bool isSequenceNumber(const QString &line)
{
    return !line.isEmpty()
           && std::all_of(line.cbegin(), line.cend(), isAsciiDigit);
}

std::optional<qint64> parseTimestamp(QStringView timestamp)
{
    if (timestamp.size() != 12 || timestamp.at(2) != QChar(':')
        || timestamp.at(5) != QChar(':') || timestamp.at(8) != QChar(',')) {
        return std::nullopt;
    }

    constexpr std::array<qsizetype, 9> digits{0, 1, 3, 4, 6, 7, 9, 10, 11};
    for (const qsizetype index : digits) {
        if (!isAsciiDigit(timestamp.at(index)))
            return std::nullopt;
    }
    const auto digit = [timestamp](qsizetype index) {
        return timestamp.at(index).unicode() - u'0';
    };
    const qint64 hours = digit(0) * 10 + digit(1);
    const int minutes = digit(3) * 10 + digit(4);
    const int seconds = digit(6) * 10 + digit(7);
    const int milliseconds = digit(9) * 100 + digit(10) * 10 + digit(11);
    if (minutes > 59 || seconds > 59)
        return std::nullopt;

    // Fixed-width fields bound the result well below qint64's limit.
    return ((hours * 60 + minutes) * 60 + seconds) * 1000 + milliseconds;
}

std::optional<TimeRange> parseTimingLine(const QString &line)
{
    const qsizetype arrow = line.indexOf(QStringLiteral("-->"));
    if (arrow < 0 || line.indexOf(QStringLiteral("-->"), arrow + 3) >= 0)
        return std::nullopt;

    const QStringView view(line);
    const auto start = parseTimestamp(view.left(arrow).trimmed());
    const auto end = parseTimestamp(view.mid(arrow + 3).trimmed());
    if (!start || !end)
        return std::nullopt;
    return TimeRange{*start, *end};
}

bool hasTimestampShape(QStringView text)
{
    text = text.trimmed();
    if (text.isEmpty())
        return false;
    const QChar first = text.front();
    return (isAsciiDigit(first) || first == QChar('-') || first == QChar('+'))
           && text.contains(QChar(':'));
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

bool startsCue(const QStringList &lines, qsizetype index)
{
    return isTimingShapedLine(lines.at(index))
           || (isSequenceNumber(lines.at(index).trimmed())
               && index + 1 < lines.size()
               && isTimingShapedLine(lines.at(index + 1)));
}

void addDiagnostic(SrtParseResult &result, SrtDiagnosticSeverity severity,
                   SrtDiagnosticCode code, qsizetype line, const QString &message)
{
    result.diagnostics.append(SrtDiagnostic{severity, code, line, message});
}

} // namespace

bool SrtParseResult::hasErrors() const
{
    return std::any_of(diagnostics.cbegin(), diagnostics.cend(),
                       [](const SrtDiagnostic &diagnostic) {
        return diagnostic.severity == SrtDiagnosticSeverity::Error;
    });
}

SrtParseResult SrtParser::parse(const QString &text)
{
    SrtParseResult result;
    QString normalized = text;
    if (normalized.startsWith(QChar(u'\ufeff')))
        normalized.remove(0, 1);
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar('\r'), QChar('\n'));
    if (normalized.trimmed().isEmpty()) {
        addDiagnostic(result, SrtDiagnosticSeverity::Warning,
                      SrtDiagnosticCode::EmptyInput, 1,
                      QStringLiteral("Input contains no subtitle entries."));
        return result;
    }

    const QStringList lines = normalized.split(QChar('\n'));
    qsizetype index = 0;
    while (index < lines.size()) {
        if (lines.at(index).trimmed().isEmpty()) {
            ++index;
            continue;
        }

        const qsizetype entryLine = index + 1;
        if (isSequenceNumber(lines.at(index).trimmed())) {
            ++index;
            if (index >= lines.size() || lines.at(index).trimmed().isEmpty()) {
                addDiagnostic(result, SrtDiagnosticSeverity::Error,
                              SrtDiagnosticCode::MissingTimingLine, entryLine,
                              QStringLiteral("Sequence number is not followed by a timing line."));
                continue;
            }
        }

        const qsizetype timingLine = index + 1;
        const QString timingText = lines.at(index).trimmed();
        const auto range = parseTimingLine(timingText);
        if (!range) {
            const qsizetype arrow = timingText.indexOf(QStringLiteral("-->"));
            if (arrow < 0 || timingText.indexOf(QStringLiteral("-->"), arrow + 3) >= 0) {
                addDiagnostic(result, SrtDiagnosticSeverity::Error,
                              SrtDiagnosticCode::InvalidTimingLine, timingLine,
                              QStringLiteral("Expected a timing line with exactly one '-->' separator."));
            } else {
                addDiagnostic(result, SrtDiagnosticSeverity::Error,
                              SrtDiagnosticCode::InvalidTimestamp, timingLine,
                              QStringLiteral("Start and end must use HH:MM:SS,mmm with ASCII digits, "
                                             "hours 00..99, minutes/seconds 00..59 and three millisecond digits."));
            }
            // Always consume the bad header, then resume at a separator or
            // a recognizable new cue so a missing blank line loses no more cues.
            ++index;
            while (index < lines.size() && !lines.at(index).trimmed().isEmpty()) {
                if (startsCue(lines, index)) {
                    addDiagnostic(result, SrtDiagnosticSeverity::Warning,
                                  SrtDiagnosticCode::MissingBlankLine, index + 1,
                                  QStringLiteral("Missing blank line before the next subtitle entry; "
                                                 "a timing-shaped line is treated as a new entry."));
                    break;
                }
                ++index;
            }
            continue;
        }
        ++index;

        QStringList textLines;
        while (index < lines.size() && !lines.at(index).trimmed().isEmpty()) {
            if (startsCue(lines, index)) {
                addDiagnostic(result, SrtDiagnosticSeverity::Warning,
                              SrtDiagnosticCode::MissingBlankLine, index + 1,
                              QStringLiteral("Missing blank line before the next subtitle entry; "
                                             "a timing-shaped line is treated as a new entry."));
                break;
            }
            textLines.append(lines.at(index));
            ++index;
        }

        if (range->endMs < range->startMs) {
            addDiagnostic(result, SrtDiagnosticSeverity::Error,
                          SrtDiagnosticCode::EndBeforeStart, timingLine,
                          QStringLiteral("Subtitle end time is earlier than its start time."));
        }
        if (textLines.isEmpty()) {
            addDiagnostic(result, SrtDiagnosticSeverity::Error,
                          SrtDiagnosticCode::EmptyText, timingLine,
                          QStringLiteral("Subtitle entry contains no text."));
        }
        if (range->endMs < range->startMs || textLines.isEmpty())
            continue;

        if (range->endMs == range->startMs) {
            addDiagnostic(result, SrtDiagnosticSeverity::Warning,
                          SrtDiagnosticCode::ZeroDuration, timingLine,
                          QStringLiteral("Subtitle start and end times are equal; the entry is retained."));
        }
        result.entries.append(SubtitleEntry{range->startMs, range->endMs,
                                            textLines.join(QChar('\n'))});
    }
    return result;
}

} // namespace asmr
