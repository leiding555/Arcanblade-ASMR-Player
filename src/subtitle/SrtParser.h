#pragma once

#include "subtitle/SubtitleEntry.h"

#include <QList>

namespace asmr {

enum class SrtDiagnosticSeverity
{
    Warning,
    Error
};

enum class SrtDiagnosticCode
{
    EmptyInput,
    MissingTimingLine,
    InvalidTimingLine,
    InvalidTimestamp,
    EndBeforeStart,
    EmptyText,
    MissingBlankLine,
    ZeroDuration
};

struct SrtDiagnostic
{
    SrtDiagnosticSeverity severity;
    SrtDiagnosticCode code;
    qsizetype line; // One-based source line, also for CRLF input.
    QString message;
};

struct SrtParseResult
{
    QList<SubtitleEntry> entries;
    QList<SrtDiagnostic> diagnostics;

    [[nodiscard]] bool hasErrors() const;
};

class SrtParser final
{
public:
    // Input is decoded text; an initial U+FEFF (UTF-8 BOM) is accepted.
    // Timestamps use HH:MM:SS,mmm: two hour digits, minutes/seconds 00..59.
    // Numeric indices are optional. Text is preserved with LF line breaks.
    // Entries stay in source order, including overlaps and zero durations.
    // Errors reject the affected entry; warnings retain usable entries.
    // Empty input is a warning, and diagnostic line numbers are one-based.
    // Missing blank lines are recovered at a timing-shaped line or at a
    // numeric line followed by a timing-shaped line. Such ambiguous text is treated
    // as the next cue and reported with MissingBlankLine.
    [[nodiscard]] static SrtParseResult parse(const QString &text);
};

} // namespace asmr
