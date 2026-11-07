#pragma once

#include "subtitle/SubtitleEntry.h"

#include <QList>

namespace asmr {

struct LrcParseOptions
{
    qint64 lastEntryDurationMs = 5000;
    qint64 maxEntryDurationMs = 10000;
};

enum class LrcDiagnosticSeverity
{
    Warning,
    Error
};

enum class LrcDiagnosticCode
{
    EmptyInput,
    InvalidOptions,
    MissingTimestamp,
    InvalidTimestamp,
    TimestampOverflow,
    MalformedTag,
    UnsupportedTag,
    UnsupportedEnhancedLrc,
    EmptyTextMarker,
    EndTimeClamped,
    EndTimeOverflow
};

struct LrcDiagnostic
{
    LrcDiagnosticSeverity severity;
    LrcDiagnosticCode code;
    qsizetype line; // One-based source line, also for CRLF input.
    QString message;
};

struct LrcParseResult
{
    QList<SubtitleEntry> entries;
    QList<LrcDiagnostic> diagnostics;

    [[nodiscard]] bool hasErrors() const;
};

class LrcParser final
{
public:
    // Decoded text accepts an initial U+FEFF and LF, CRLF or CR line endings.
    // Prefix timestamps use mm+:ss.xx or mm+:ss.xxx: at least two ASCII minute
    // digits, seconds 00..59, and exactly two (centisecond) or three millisecond
    // fractional digits. Minutes may exceed 59; times must fit qint64.
    // Each tagged line supplies one literal lyric, expanded for every valid
    // prefix timestamp in source order. Untagged nonblank lines are errors,
    // never continuations. Text whitespace, Unicode, markup and entities stay
    // literal. Leading spaces/tabs before the first tag are accepted.
    // Metadata ti/ar/al/by/au/re/ve/length is ignored (case-insensitive).
    // Other tags, including offset, warn and have no effect. Closed invalid
    // tags do not discard valid neighboring timestamps. An unclosed prefix
    // tag rejects that line because the lyric boundary cannot be determined.
    // Enhanced LRC word timestamps <mm:ss[.xx[x]]> reject the affected line.
    //
    // End times use the next strictly greater start from all valid timestamps,
    // capped by maxEntryDurationMs. The final timestamp instead uses
    // min(lastEntryDurationMs, maxEntryDurationMs). Both options must be positive.
    // Empty/whitespace lyrics are clear markers: they contribute boundaries,
    // warn, and produce no entry. Equal timestamps can display simultaneously,
    // including a nonempty lyric sharing a clear marker's time.
    // Arithmetic saturates at qint64 max with a warning; a start at qint64 max
    // cannot form a positive interval and is rejected. Entries use [start,end).
    // These duration/clear-marker policies are application choices, not LRC
    // specification requirements. Final entries always retain input order.
    [[nodiscard]] static LrcParseResult parse(
        const QString &text, const LrcParseOptions &options = {});
};

} // namespace asmr
