#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace asmr {

inline constexpr qint64 WorkReferenceTextBytes = 4 * 1024 * 1024;
inline constexpr qint64 WorkReferencePdfBytes = 32 * 1024 * 1024;
inline constexpr int WorkReferencePdfPages = 200;
inline constexpr qsizetype WorkReferenceCharacters = 1'000'000;
inline constexpr qsizetype WorkReferenceMaximumFiles = 1'000;
inline constexpr qint64 WorkReferenceWorkBytes = 32 * 1024 * 1024;
inline constexpr auto WorkReferenceExtractorVersion = "work-reference-v1";

enum class WorkReferenceRefreshPolicy { ReuseCache, ForceReextract };

struct WorkReferenceCandidate {
    QString absolutePath, relativePath, format;
    QString source = QStringLiteral("file"); // file / external-subtitle / generated-asr
    QString audioRelativePath;
    QDateTime completedAt;
    QString warning;
    bool operator==(const WorkReferenceCandidate &) const = default;
};

struct WorkReferenceSelection {
    bool success = false;
    bool cancelled = false;
    QString error;
    QVector<WorkReferenceCandidate> files;
    QStringList excludedPaths;
    QStringList warnings;
};

struct WorkReferenceRecord {
    qint64 id = 0, workId = 0;
    QString relativePath, format, source, audioRelativePath;
    QDateTime completedAt;
    QString sourceHash, encoding, extractorVersion, contentVersion;
    bool encodingInferred = false;
    QJsonObject content;
    QString state, error, warning; // ready / no-text / failed; missing is read-only legacy compatibility.
    bool missing = false;
    bool cacheReused = false; // Runtime accounting only.
    qint64 subtitleFileId = 0, audioFileId = 0; // Runtime library identities for ASR context.
};

struct WorkReferenceSnapshot {
    bool success = false;
    bool scanned = false;
    QString synchronizedAt, error;
    QVector<WorkReferenceRecord> records;
    bool asrContext = false;
    QStringList warnings;
};

enum class WorkReferenceSyncStatus { NotRun, Succeeded, Failed, Cancelled };
struct WorkReferenceSyncResult {
    WorkReferenceSyncStatus status = WorkReferenceSyncStatus::NotRun;
    qsizetype selectedCount = 0, readyCount = 0, failedCount = 0;
    qsizetype removedCount = 0, reusedCount = 0;
    QString error;
    QStringList warnings;
};

} // namespace asmr
