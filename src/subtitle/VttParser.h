#pragma once

#include "subtitle/SubtitleEntry.h"

#include <QList>

namespace asmr {

enum class VttDiagnosticSeverity
{
    Warning,
    Error
};

enum class VttDiagnosticCode
{
    EmptyInput,
    InvalidHeader,
    MissingHeaderSeparator,
    MissingTimingLine,
    InvalidTimingLine,
    InvalidTimestamp,
    EndBeforeStart,
    EmptyText,
    ZeroDuration,
    UnsupportedHeaderMetadata,
    IgnoredCueSettings,
    IgnoredStyleBlock,
    IgnoredRegionBlock,
    LiteralInlineMarkup,
    MissingBlankLine
};

struct VttDiagnostic
{
    VttDiagnosticSeverity severity;
    VttDiagnosticCode code;
    qsizetype line; // One-based source line, also for CRLF input.
    QString message;
};

struct VttParseResult
{
    QList<SubtitleEntry> entries;
    QList<VttDiagnostic> diagnostics;

    [[nodiscard]] bool hasErrors() const;
};

class VttParser final
{
public:
    // Decoded UTF-8 text, with an optional initial U+FEFF, must start with WEBVTT
    // (case-insensitive).
    // Times use MM:SS.mmm or HH+:MM:SS.mmm (at least two hour digits), with
    // minutes/seconds 00..59; values must fit qint64 integer milliseconds.
    // Identifiers are accepted but not stored. Source order, multiline text,
    // overlaps and zero-duration entries are retained with [startMs, endMs).
    // NOTE blocks are skipped. STYLE/REGION blocks and cue settings are ignored
    // with warnings. Header metadata is ignored with warnings.
    // Cue payload is literal text: tags, entities and inline timestamps are not
    // interpreted. A '<' or an '&...;' sequence warns once per cue.
    // A missing blank line is recovered at a timing-shaped line; such a line
    // inside cue text is ambiguous and is treated as the next cue. An identifier
    // without a preceding blank line remains part of the previous cue's text.
    // An invalid file header rejects the document; other errors reject a cue.
    [[nodiscard]] static VttParseResult parse(const QString &text);
};

} // namespace asmr
