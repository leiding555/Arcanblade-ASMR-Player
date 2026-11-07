#pragma once

#include "library/WorkReferenceTypes.h"

#include <QString>
#include <QVector>

#include <functional>

namespace asmr {

enum class LibraryFileType { Audio, Subtitle };

struct DiscoveredFile {
    QString absolutePath;
    QString relativePath;
    QString fileName;
    QString extension; // Lowercase, without the leading dot.
    LibraryFileType type;

    bool operator==(const DiscoveredFile &) const = default;
};

enum class DirectoryScanStatus {
    Completed,
    EmptyDirectory,
    NoSupportedFiles,
    PartialFailure,
    Failed,
    Cancelled,
    LimitExceeded,
};

enum class ScanDiagnosticSeverity { Info, Warning, Error };

enum class ScanDiagnosticCode {
    InvalidPath,
    InvalidOptions,
    DirectoryNotFound,
    NotDirectory,
    AccessDenied,
    DirectoryReadFailed,
    MetadataFailed,
    LinkSkipped,
    UnsupportedFile,
    UnsupportedEntry,
    DepthLimitReached,
    EntryLimitReached,
    Cancelled,
};

struct ScanDiagnostic {
    ScanDiagnosticSeverity severity;
    ScanDiagnosticCode code;
    QString absolutePath;
    QString relativePath;
    QString message;
};

struct DirectoryScanOptions {
    int maximumDepth = 64; // Root depth is zero.
    qsizetype maximumEntries = 100'000; // All entries, not only supported files.
    qsizetype maximumDiagnostics = 1'000;
};

struct DirectoryScanResult {
    QString rootDirectory; // Absolute, cleaned Qt path with forward slashes.
    DirectoryScanStatus status = DirectoryScanStatus::Failed;
    QVector<DiscoveredFile> audioFiles;
    QVector<DiscoveredFile> subtitleFiles;
    // Auxiliary sources do not change media support or import eligibility.
    QVector<WorkReferenceCandidate> referenceFiles;
    QVector<ScanDiagnostic> diagnostics;
    qsizetype visitedEntries = 0;
    qsizetype skippedEntries = 0;
    qsizetype suppressedDiagnostics = 0;

    // PartialFailure retains explicitly incomplete discoveries. Cancelled,
    // LimitExceeded and Failed expose no discovered file records.
    bool isComplete() const;
};

class DirectoryScanner final {
public:
    // Synchronous and GUI independent. A future worker can pass a callback that
    // reads its caller-owned atomic cancellation flag; this class creates no
    // threads. The callback must not throw or perform GUI work.
    static DirectoryScanResult scan(
        const QString &directory,
        const DirectoryScanOptions &options = {},
        const std::function<bool()> &cancellationRequested = {});
};

} // namespace asmr
