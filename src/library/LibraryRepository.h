#pragma once

#include "library/DirectoryScanner.h"
#include "library/SqliteWriteTransaction.h"

#include <QSqlDatabase>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

class QThread;

namespace asmr {

enum class LibraryDiagnosticCode {
    DatabaseError,
    NotOpen,
    WrongThread,
    InvalidDatabasePath,
    UnsupportedSchemaVersion,
    InvalidSchema,
    RejectedScan,
    NoAudioFiles,
    InvalidScanRecord,
    PathConflict,
    InvalidSubtitleBinding,
    SubtitleNotBound,
    InvalidWorkSnapshot,
    StaleWorkSnapshot,
    AutoAssociationError,
    InvalidPlaybackProgress,
    StalePlaybackProgress,
    InvalidWorkTitle,
};

struct LibraryDiagnostic {
    LibraryDiagnosticCode code;
    QString path;
    QString message;
};

struct LibraryWork {
    qint64 id = 0;
    QString rootPath;
    QString displayName;
    qint64 importedAtMs = 0; // First successful import, UTC milliseconds.

    bool operator==(const LibraryWork &) const = default;
};

struct LibraryFile {
    qint64 id = 0;
    qint64 workId = 0;
    QString relativePath;
    QString fileName;
    QString extension;
    LibraryFileType type = LibraryFileType::Audio;
    bool missing = false;

    bool operator==(const LibraryFile &) const = default;
};

struct LibraryOperationResult {
    bool success = false;
    QVector<LibraryDiagnostic> diagnostics;
};

struct LibraryWorksResult : LibraryOperationResult {
    QVector<LibraryWork> works;
};

struct LibraryFilesResult : LibraryOperationResult {
    QVector<LibraryFile> files;
};

// Coherent database-only snapshot, including missing historical files. The file
// order is relative path (SQLite BINARY), then stable file ID.
struct LibraryWorkSnapshotResult : LibraryOperationResult {
    LibraryWork work;
    QVector<LibraryFile> files;
};

// A single database read snapshot for directory browsing. Only persisted
// bindings whose audio and subtitle are scan-present contribute to the set;
// the independent subtitle preference and transient candidates are not read.
struct LibraryWorkBrowseResult : LibraryWorkSnapshotResult {
    QSet<qint64> subtitleAudioFileIds;
};

struct AutoSubtitleBinding {
    qint64 audioFileId = 0;
    qint64 subtitleFileId = 0;

    bool operator==(const AutoSubtitleBinding &) const = default;
};

struct LibraryAutoBindingResult : LibraryOperationResult {
    qsizetype insertedBindings = 0;
    qsizetype existingBindings = 0; // Includes preserved Manual relationships.
    qsizetype manualBindingsPreserved = 0;
};

struct LibraryWorkArtworkResult : LibraryOperationResult {
    QString relativePath;
    QString managedFile;
};

struct LibraryImportResult : LibraryOperationResult {
    qint64 workId = 0;
    bool created = false;
    bool removed = false; // Committed library removal; workId identifies the deleted work.
    qsizetype insertedFiles = 0;
    qsizetype updatedFiles = 0;
    qsizetype missingFiles = 0; // Total missing records after this import.
};

// Persisted relationships are distinct from SubtitleMatcher's transient candidates.
// No language/score is persisted: a filename suffix does not verify actual language.
enum class SubtitleBindingSource { Auto = 0, Manual = 1 };
// New GUI relationships require current scan-present endpoints. The default
// preserves the repository's existing historical/missing identity operations.
enum class SubtitleBindingAvailabilityPolicy { AllowMissing, RequirePresent };
enum class SubtitleAvailability {
    NotApplicable, Available, AudioMissing, SubtitleMissing, BothMissing,
};
enum class SubtitlePreferenceState { Unset, None, Subtitle };
// Choosing an existing binding may preserve its provenance independently of
// confirming that relationship as Manual. No policy creates a new binding.
enum class SubtitlePreferenceBindingPolicy { ConfirmManual, PreserveBindingSource };

struct LibrarySubtitleBinding {
    qint64 workId = 0;
    qint64 audioFileId = 0;
    qint64 subtitleFileId = 0;
    SubtitleBindingSource source = SubtitleBindingSource::Auto;
    // Based on persisted missing flags, not a live filesystem/readability probe.
    SubtitleAvailability availability = SubtitleAvailability::NotApplicable;

    bool operator==(const LibrarySubtitleBinding &) const = default;
};

struct LibrarySubtitlePreference {
    qint64 audioFileId = 0;
    SubtitlePreferenceState state = SubtitlePreferenceState::Unset;
    qint64 subtitleFileId = 0; // Positive only for Subtitle state.
    SubtitleAvailability availability = SubtitleAvailability::NotApplicable;

    bool operator==(const LibrarySubtitlePreference &) const = default;
};

struct LibrarySubtitleBindingsResult : LibraryOperationResult {
    QVector<LibrarySubtitleBinding> bindings; // Ascending subtitle stable ID.
};

struct LibrarySubtitlePreferenceResult : LibraryOperationResult {
    LibrarySubtitlePreference preference;
};

// One read transaction: stable work/files, all bindings and the preference.
struct LibraryTrackSubtitlesResult : LibraryOperationResult {
    LibraryWork work;
    LibraryFile audio;
    QVector<LibraryFile> files;
    QVector<LibrarySubtitleBinding> bindings;
    LibrarySubtitlePreference preference;
};

// A stored observation, not a promise that this position is still seekable.
// Duration is optional; a known duration of zero is different from unknown.
struct LibraryPlaybackProgress {
    qint64 audioFileId = 0;
    qint64 positionMs = 0;
    std::optional<qint64> durationMs;
    qint64 updatedAtMs = 0; // UTC wall clock; ordering uses revision, not this time.
    qint64 revision = 0;

    bool operator==(const LibraryPlaybackProgress &) const = default;
};

struct LibraryPlaybackProgressResult : LibraryOperationResult {
    std::optional<LibraryPlaybackProgress> progress; // Empty on successful no-record/clear query.
    qint64 revision = 0; // No row = 0; cleared rows retain their ordering revision.
};

struct LibraryPlaybackProgressWriteResult : LibraryOperationResult {
    qint64 revision = 0; // New committed revision on success only; never a token on failure.
};

// Synchronous repository. Construct, use, close and destroy on the same
// thread; create a separate repository/connection in each future worker.
// Subtitle cleanup additionally probes absence on that worker. No connection
// or query object is exposed to callers.
class LibraryRepository final {
public:
    explicit LibraryRepository(QString databasePath);
    ~LibraryRepository();

    LibraryRepository(const LibraryRepository &) = delete;
    LibraryRepository &operator=(const LibraryRepository &) = delete;
    LibraryRepository(LibraryRepository &&) = delete;
    LibraryRepository &operator=(LibraryRepository &&) = delete;

    LibraryOperationResult open();
    LibraryOperationResult close();
    bool isOpen() const;
    LibraryWorksResult works();
    LibraryFilesResult files(qint64 workId);
    LibraryWorkSnapshotResult workSnapshot(qint64 workId);
    LibraryWorkBrowseResult workBrowseSnapshot(qint64 workId);

    // Compare-and-set only display_name under a write lock. File changes from
    // a rescan do not invalidate this edit, but identity or old title changes do.
    // The new single-line title is trimmed; blank or NUL-containing text fails.
    // Related external state may be saved after the SQL update and before its
    // commit. A failed applyRelated must restore its own partial effects. After
    // a successful applyRelated, a SQL failure invokes rollbackRelated; provide
    // it whenever the callback changes external state. Callbacks must not
    // reenter this repository or access its connection, and must not throw.
    LibraryOperationResult updateWorkTitle(
        const LibraryWork &expected, const QString &title,
        const std::function<LibraryOperationResult()> &applyRelated = {},
        const std::function<LibraryOperationResult()> &rollbackRelated = {});

    // Artwork is owned by the exact work import identity. Read mismatches have
    // no selection; writes reject deleted/replaced identities. No media changes.
    LibraryWorkArtworkResult workArtwork(const LibraryWork &identity);
    LibraryOperationResult setWorkArtwork(const LibraryWork &identity,
        const QString &relativePath, const QString &managedFile = {});
    // Compare-and-set title and artwork commit in one SQLite transaction.
    LibraryOperationResult updateWorkTitleAndArtwork(const LibraryWork &expected,
        const QString &title, const QString &relativePath, const QString &managedFile,
        const std::function<LibraryOperationResult()> &beforeSave = {});

    // Add-only atomic batch, intended for allowed SubtitleMatcher suggestions.
    // Rechecks the exact complete snapshot under a SQLite write lock before any
    // mutation. Pass an unmodified workSnapshot result, including its file order.
    // Missing endpoints are rejected. Duplicate pairs count once;
    // existing Manual sources and every preference decision remain unchanged.
    // Failure reports zero mutation counters, even after a rolled-back insert.
    LibraryAutoBindingResult addAutoSubtitleBindings(
        const LibraryWorkSnapshotResult &expectedSnapshot,
        const QVector<AutoSubtitleBinding> &bindings);

    // Pure input validation, without opening or changing a database. Existing
    // database identity conflicts still require importScan's transaction.
    static LibraryOperationResult validateScan(const DirectoryScanResult &scan,
                                              const QString &displayName = {});

    // Only a complete scan with at least one audio file can change the DB.
    // Failure exposes diagnostics, never successful partial mutation counters.
    LibraryImportResult importScan(const DirectoryScanResult &scan,
                                   const QString &displayName = {});

    // Existing-work refresh accepts complete scans including zero audio. It
    // compares the complete unmodified snapshot under a write lock.
    // Retained files keep their IDs, relationships, preferences and progress.
    // Confirmed absent subtitles and their bindings/defaults are removed.
    LibraryImportResult refreshExistingWork(const LibraryWorkSnapshotResult &expected,
                                            const DirectoryScanResult &scan);
    // Removes only the expected work and its files/bindings/preferences/progress.
    // The worker rechecks filesystem absence with verifyAvailability after the
    // write lock and snapshot check, before any deletion. It must not reenter
    // this repository or mutate its database connection.
    // Database-only callers may omit it after independently confirming absence.
    // AUTOINCREMENT identities are never reused, including a later same-path import.
    LibraryImportResult removeMissingWork(
        const LibraryWorkSnapshotResult &expected,
        const std::function<LibraryOperationResult()> &verifyAvailability = {});

    // Explicit user removal is database-only and accepts an existing directory.
    // The complete expected snapshot is checked under the same write lock and
    // uses the same atomic cleanup as removeMissingWork. No local files change.
    // An optional final check runs after identity validation, before deletion;
    // it must not reenter this repository or mutate its database connection.
    LibraryImportResult removeWork(
        const LibraryWorkSnapshotResult &expected,
        const std::function<LibraryOperationResult()> &beforeRemoval = {});

    // Every write is atomic. Auto may add relationships, but cannot demote Manual
    // or change a user preference. Confirming an Auto binding upgrades it to Manual.
    // Existing missing records remain valid identities; availability is queried
    // from the latest import's missing flags, never from filenames or live disk.
    // RequirePresent checks both persisted missing flags under the transaction's
    // write lock; this is not a live filesystem or subtitle-loading check.
    LibraryOperationResult bindSubtitle(
        qint64 audioFileId, qint64 subtitleFileId, SubtitleBindingSource source,
        SubtitleBindingAvailabilityPolicy availabilityPolicy = SubtitleBindingAvailabilityPolicy::AllowMissing);
    LibrarySubtitleBindingsResult subtitleBindings(qint64 audioFileId);
    LibraryTrackSubtitlesResult trackSubtitles(qint64 workId, qint64 audioFileId);
    // Worker-only filesystem check followed by a short atomic cleanup. Only
    // confirmed absent subtitle files are removed; offline or inaccessible
    // directories retain their records. Related bindings and explicit defaults
    // are removed together. Repeated cleanup of already removed IDs is harmless.
    LibraryOperationResult pruneMissingSubtitles(const LibraryWorkSnapshotResult &expected);
    // Explicit removal clears a preference referring to this binding to Unset
    // in the same transaction. Removing another binding preserves the preference.
    LibraryOperationResult removeSubtitleBinding(qint64 audioFileId, qint64 subtitleFileId);
    // Requires an existing binding and atomically stores the choice. The default
    // confirms it as Manual; PreserveBindingSource writes only the preference.
    // Missing bound subtitles may be selected without losing history.
    LibraryOperationResult setPreferredSubtitle(
        qint64 audioFileId, qint64 subtitleFileId,
        SubtitlePreferenceBindingPolicy bindingPolicy = SubtitlePreferenceBindingPolicy::ConfirmManual);
    // User selection requires present same-work endpoints. Atomically creates a
    // Manual binding when absent and saves the preference; existing provenance
    // is preserved. The caller performs live path/readability validation.
    LibraryOperationResult useSubtitle(qint64 audioFileId, qint64 subtitleFileId);
    LibraryOperationResult setNoSubtitlePreference(qint64 audioFileId);
    LibraryOperationResult clearSubtitlePreference(qint64 audioFileId);
    LibrarySubtitlePreferenceResult subtitlePreference(qint64 audioFileId);

    // Synchronous optimistic writes: pass the revision returned by the latest
    // query or successful write. Zero means never written. A stale token fails
    // without mutation; never retry an old payload with a freshly read token.
    // Missing audio records remain valid. Position/duration are nonnegative
    // qint64 observations; position may exceed an outdated known duration.
    LibraryPlaybackProgressResult playbackProgress(qint64 audioFileId);
    LibraryPlaybackProgressWriteResult savePlaybackProgress(
        qint64 audioFileId, qint64 positionMs, std::optional<qint64> durationMs,
        qint64 expectedRevision);
    // Clears the recovery value but retains/increments its revision to prevent
    // a delayed save from resurrecting it. Clearing no record also advances it.
    LibraryPlaybackProgressWriteResult clearPlaybackProgress(qint64 audioFileId,
                                                             qint64 expectedRevision);

    // Per-instance deterministic concurrency hooks; production keeps defaults.
    void setWriteTransactionOptionsForTesting(const SqliteWriteTransaction::Options &options)
    { m_writeTransactionOptions = options; }

private:
    LibraryWorkBrowseResult readWorkSnapshot(qint64 workId, bool includeSubtitleSummary);
    LibraryImportResult synchronizeScan(const DirectoryScanResult &scan,
                                         const QString &displayName,
                                         const LibraryWorkSnapshotResult *expected);
    LibraryImportResult removeExpectedWork(
        const LibraryWorkSnapshotResult &expected,
        const std::function<LibraryOperationResult()> &beforeRemoval);
    bool onOwnerThread() const;
    LibraryOperationResult checkReady() const;
    LibraryOperationResult prepareSchema(bool writing = false);
    LibraryOperationResult writeSubtitleChange(
        const std::function<LibraryOperationResult()> &change);
    static bool confirmedMissingSubtitle(const QString &rootPath, const LibraryFile &file);
    LibraryOperationResult removeConfirmedMissingSubtitleRows(
        const QString &rootPath, const QVector<LibraryFile> &files);
    LibraryPlaybackProgressWriteResult writePlaybackProgressChange(
        qint64 audioFileId, std::optional<qint64> positionMs,
        std::optional<qint64> durationMs, qint64 expectedRevision);
    void releaseConnection();

    QString m_databasePath;
    QString m_connectionName;
    QThread *m_ownerThread = nullptr;
    QSqlDatabase m_database;
    SqliteWriteTransaction::Options m_writeTransactionOptions;
};

} // namespace asmr
