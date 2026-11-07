#pragma once

#include "subtitle/SubtitleEntry.h"

#include <QList>
#include <QString>
#include <QtGlobal>

namespace asmr {

enum class SubtitleFormat
{
    Srt,
    Vtt,
    Lrc
};

enum class SubtitleFileError
{
    None,
    OpenFailed,
    ReadFailed,
    TooLarge,
    InvalidUtf8,
    NoValidEntries,
    UnsupportedFormat,
    ExpandedTextTooLarge
};

enum class SubtitleLoadDiagnosticSeverity
{
    Warning,
    Error
};

// Format-neutral diagnostics for callers; parser-specific codes remain available
// on SrtParseResult / VttParseResult / LrcParseResult for the text parsers.
struct SubtitleLoadDiagnostic
{
    SubtitleLoadDiagnosticSeverity severity;
    qsizetype line; // One-based source line.
    QString message;
};

struct SubtitleFileLoadResult
{
    SubtitleFileError error = SubtitleFileError::None;
    QString errorMessage;
    QList<SubtitleEntry> entries;
    QList<SubtitleLoadDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const;
};

class SubtitleFileLoader final
{
public:
    // Byte limit includes an optional UTF-8 BOM.
    static constexpr qint64 MaxFileSizeBytes = 4 * 1024 * 1024;
    // Sum of entry.text.size() in UTF-16 code units, including repeated lyrics.
    // Bounds derived text before overlapping entries can be joined by the GUI.
    static constexpr qint64 MaxExpandedTextCodeUnits = 4 * 1024 * 1024;

    // Reads only ordinary local files. UTF-8 is validated before parsing.
    // The caller selects the format explicitly; no content/encoding guessing.
    // Usable entries within both resource limits constitute success; all parser
    // diagnostics are retained. Expanded text overflow rejects the entire load.
    // Synchronous work: GUI callers should invoke this outside the GUI thread.
    [[nodiscard]] static SubtitleFileLoadResult load(const QString &path,
                                                    SubtitleFormat format);
};

} // namespace asmr
