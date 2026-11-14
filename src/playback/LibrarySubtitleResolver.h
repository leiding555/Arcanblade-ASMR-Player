#pragma once

#include "library/LibraryRepository.h"
#include "subtitle/GeneratedSubtitleInfo.h"
#include "subtitle/SubtitleFileLoader.h"

namespace asmr {

enum class LibrarySubtitleDecision { NoAvailable, Disabled, NeedsSelection, Selected, Error };
enum class LibrarySubtitleIssueCode { Database, Identity, Missing, UnsafePath, Unreadable, Unsupported };
struct LibrarySubtitleIssue {
    LibrarySubtitleIssueCode code;
    qint64 subtitleFileId = 0;
    QString path;
    QString message;
};
struct LibrarySubtitlePathResult {
    bool success = false;
    QString path;
    SubtitleFormat format = SubtitleFormat::Srt;
    LibrarySubtitleIssue issue;
};
struct LibrarySubtitleResolution {
    LibrarySubtitleDecision decision = LibrarySubtitleDecision::Error;
    LibraryTrackSubtitlesResult snapshot;
    QVector<qint64> availableSubtitleIds;
    LibraryFile selectedFile;
    QVector<LibrarySubtitleIssue> issues;
    bool pruned = false;
};
struct LibrarySubtitleOrderKey {
    QDateTime time;
    bool chinese = false;
    QString relativePath;
    qint64 id = 0;
};

// The key reads filesystem metadata. Build it alongside path validation on a worker.
LibrarySubtitleOrderKey librarySubtitleOrderKey(const QString &validatedPath, const LibraryFile &file,
                                               const std::optional<GeneratedSubtitleInfo> &generation);
// Shared by automatic selection and the bound-subtitle list. Pure comparison.
inline bool librarySubtitleNewer(const LibrarySubtitleOrderKey &left, const LibrarySubtitleOrderKey &right)
{
    if (left.time.isValid() != right.time.isValid())
        return left.time.isValid();
    if (left.time != right.time)
        return left.time > right.time;
    if (left.chinese != right.chinese)
        return left.chinese;
#ifdef Q_OS_WIN
    const int folded = QString::compare(left.relativePath, right.relativePath, Qt::CaseInsensitive);
    if (folded != 0)
        return folded < 0;
#endif
    const int path = QString::compare(left.relativePath, right.relativePath, Qt::CaseSensitive);
    return path != 0 ? path < 0 : left.id < right.id;
}

// Synchronous, GUI-free helpers. Call outside the GUI thread.
LibrarySubtitlePathResult validateLibrarySubtitlePath(const QString &root, const LibraryFile &file);
LibrarySubtitleResolution resolveLibrarySubtitles(const QString &databasePath,
                                                 qint64 workId, qint64 audioFileId);
LibrarySubtitleResolution selectLibrarySubtitles(LibraryTrackSubtitlesResult snapshot);
// Recheck containment/readability immediately before and after the existing loader.
SubtitleFileLoadResult loadLibrarySubtitle(const QString &root, const LibraryFile &file);

} // namespace asmr
