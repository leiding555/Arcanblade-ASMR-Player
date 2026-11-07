#pragma once

#include "library/DirectoryScanner.h"
#include "library/LibraryRepository.h"
#include "library/SubtitleMatcher.h"
#include "library/WorkReferenceTypes.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

namespace asmr {

enum class LibraryImportState {
    Idle,
    Scanning,
    Committing,
    ProcessingReferences,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

enum class LibraryImportSubmissionStatus { Accepted, Busy, ShuttingDown, WrongThread };

struct LibraryImportSubmission {
    LibraryImportSubmissionStatus status = LibraryImportSubmissionStatus::Busy;
    QString taskId;
};

enum class LibraryImportCancelResult {
    Requested,
    AlreadyRequested,
    TooLate,
    NoActiveTask,
    WrongThread,
};

enum class LibraryAutoAssociationStatus { NotRun, Succeeded, Failed, StaleSnapshot };

struct LibraryAutoAssociationResult {
    LibraryAutoAssociationStatus status = LibraryAutoAssociationStatus::NotRun;
    qint64 workId = 0;
    SubtitleMatchResult matches; // Retain structured candidate rules and skip reasons.
    qsizetype candidateCount = 0;
    qsizetype eligibleBindings = 0;
    qsizetype skippedWeakCandidates = 0;
    qsizetype skippedAmbiguousCandidates = 0;
    // Subsets of skippedAmbiguousCandidates; these two may overlap.
    qsizetype skippedCompetingCandidates = 0;
    qsizetype skippedVersionAmbiguityCandidates = 0;
    // Committed batch counters only; all remain zero on a failed/stale batch.
    qsizetype addedBindings = 0;
    qsizetype existingBindings = 0;
    qsizetype manualBindingsPreserved = 0; // Subset of existingBindings.
    qint64 matchingElapsedNs = 0; // Includes the matcher's filesystem metadata checks.
    qint64 bindingElapsedNs = 0; // Includes snapshot validation, lock wait and commit.
    QVector<LibraryDiagnostic> diagnostics;
};
struct LibraryImportTaskResult {
    QString taskId;
    LibraryImportState state = LibraryImportState::Failed;
    DirectoryScanResult scan;
    LibraryImportResult repositoryResult;
    bool databaseCommitted = false;
    QString errorMessage;
    LibraryAutoAssociationResult autoAssociation;
    WorkReferenceSyncResult references;
};

class LibraryImportThread;
class LibraryImportServiceTestAccess;

// Submit/cancel/shutdown and all observations belong to this QObject's thread.
// Only ordinary value data crosses threads; the worker owns its repository.
// Prefer asynchronous shutdown() before destruction. The destructor cooperates
// with cancellation and joins, which can wait for a filesystem call, import or association batch.
class LibraryImportService final : public QObject {
    Q_OBJECT

public:
    explicit LibraryImportService(QString databasePath, QObject *parent = nullptr);
    ~LibraryImportService() override;

    LibraryImportSubmission submit(const QString &directory,
                                   const DirectoryScanOptions &options = {},
                                   const QString &displayName = {});
    LibraryImportSubmission submitRefresh(qint64 workId, const QString &directory,
                                          const DirectoryScanOptions &options = {},
                                          WorkReferenceRefreshPolicy references = WorkReferenceRefreshPolicy::ReuseCache);
    LibraryImportSubmission submitRemoval(qint64 workId, const QString &directory,
                                          const QString &scanRoot);
    // User-confirmed library-only removal. Does not scan or delete local files;
    // the supplied original path must still match the persisted work identity.
    LibraryImportSubmission submitManualRemoval(qint64 workId, const QString &directory);
    LibraryImportCancelResult cancelCurrent();
    bool shutdown(); // Permanent, nonblocking; false only for a wrong-thread call.

    LibraryImportState state() const { return m_state; }
    QString activeTaskId() const { return m_activeTaskId; }
    bool isBusy() const { return m_thread != nullptr || m_finalizing; }
    bool isShuttingDown() const { return m_shuttingDown; }

signals:
    void stateChanged(QString taskId, asmr::LibraryImportState state);
    // A real end-of-scan snapshot, not a fabricated live percentage.
    void scanCompleted(QString taskId, asmr::DirectoryScanResult scan);
    // Durable work import/refresh/removal; removed results retain the old work ID.
    // Ordinary imports/refreshes precede the independent association batch.
    void importCommitted(QString taskId, asmr::LibraryImportResult result);
    // Emitted once, after the worker and its SQL connection have stopped.
    void taskFinished(asmr::LibraryImportTaskResult result);
    void shutdownFinished();

private:
    friend class LibraryImportThread;
    friend class LibraryImportServiceTestAccess;

    enum class TaskKind { Import, Refresh, Removal, ManualRemoval };
    LibraryImportSubmission submitTask(TaskKind kind, qint64 workId,
                                       const QString &directory,
                                       const DirectoryScanOptions &options,
                                       const QString &displayName, const QString &scanRoot,
                                       WorkReferenceRefreshPolicy references = WorkReferenceRefreshPolicy::ReuseCache);
    LibraryImportCancelResult requestCancellation();

    enum class TaskPhase { Scanning, CancelRequested, Committing, CommittingStopRequested,
                           ProcessingReferences, ReferenceCommitting, Finished };
    struct TaskControl {
        std::atomic<TaskPhase> phase{TaskPhase::Scanning};
        std::atomic_bool referencesCancelled{false};
    };

    // Narrow, private deterministic test seam; production leaves this empty.
    enum class TestPoint { BeforeScan, AfterScan, BeforeCommit, Committing,
                           RepositoryOpened, BeforeFinished, ImportCommitted, AfterMatching,
                           RemovalBeforeLock, RemovalLocked, BeforeReferences, ReferencesCommitting };
    struct TestHooks {
        std::function<void(TestPoint)> reached;
        std::function<void(DirectoryScanResult &)> afterScan;
    };

    void applyScanCompleted(const QString &taskId, const DirectoryScanResult &scan);
    void applyCommitting(const QString &taskId);
    void applyProcessingReferences(const QString &taskId);
    void applyImportCommitted(const QString &taskId, const LibraryImportResult &result);
    void finishTask(const QString &taskId, LibraryImportThread *worker);
    void finishShutdownIfReady();

    QString m_databasePath;
    QString m_activeTaskId;
    LibraryImportState m_state = LibraryImportState::Idle;
    LibraryImportThread *m_thread = nullptr;
    std::shared_ptr<TaskControl> m_control;
    std::shared_ptr<const TestHooks> m_testHooks;
    bool m_finalizing = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::DirectoryScanResult)
Q_DECLARE_METATYPE(asmr::LibraryImportState)
Q_DECLARE_METATYPE(asmr::LibraryImportResult)
Q_DECLARE_METATYPE(asmr::LibraryImportTaskResult)

Q_DECLARE_METATYPE(asmr::LibraryImportSubmission)
