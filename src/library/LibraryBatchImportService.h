#pragma once

#include "library/LibraryImportService.h"
#include "library/WorkDirectoryDiscovery.h"

#include <QFuture>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>

namespace asmr {

enum class LibraryBatchImportState { Idle, Discovering, AwaitingSelection, Importing, Stopping, Finished };
enum class LibraryBatchItemOutcome { Imported, ImportedAssociationFailed, Skipped, Failed, Cancelled, NotStarted };

struct LibraryBatchImportOptions {
    WorkDirectoryDiscoveryOptions discovery;
    DirectoryScanOptions scan;
    qsizetype maximumItemDiagnostics = 20;
};

// A bounded summary only: no file snapshot, subtitle candidates or media content.
struct LibraryBatchImportItemResult {
    QString directory;
    qint64 workId = 0;
    LibraryBatchItemOutcome outcome = LibraryBatchItemOutcome::NotStarted;
    bool created = false;
    qint64 insertedFiles = 0;
    qint64 updatedFiles = 0;
    qint64 missingFiles = 0;
    LibraryAutoAssociationStatus associationStatus = LibraryAutoAssociationStatus::NotRun;
    qsizetype addedBindings = 0;
    qsizetype existingBindings = 0;
    qsizetype manualBindingsPreserved = 0;
    QStringList diagnostics;
    qsizetype suppressedDiagnostics = 0;
    QString errorMessage;
};

struct LibraryBatchImportResult {
    QString batchId;
    QString rootDirectory;
    bool cancelled = false;
    QString errorMessage;
    QStringList diagnostics;
    qsizetype suppressedDiagnostics = 0;
    qsizetype rootFilesSkipped = 0;
    qsizetype linksSkipped = 0;
    qsizetype selectedCount = 0;
    qsizetype imported = 0;
    qsizetype associationFailed = 0;
    qsizetype skipped = 0;
    qsizetype failed = 0;
    qsizetype cancelledItems = 0;
    qsizetype notStarted = 0;
    QVector<LibraryBatchImportItemResult> items;
};

class LibraryBatchImportServiceTestAccess;

// Own-thread coordinator. The supplied single-work service is borrowed and is
// the only writer. A preview never submits a work; startSelected accepts only
// exact, unique paths from that batch's preview, in preview order.
// Call shutdown() before destruction to wait asynchronously for discovery and
// an already accepted child. Destructor cancels/joins discovery as a fallback;
// the borrowed single service remains responsible for its own worker lifetime.
class LibraryBatchImportService final : public QObject {
    Q_OBJECT
public:
    explicit LibraryBatchImportService(QString databasePath, LibraryImportService *single,
                                       QObject *parent = nullptr);
    ~LibraryBatchImportService() override;

    LibraryImportSubmission discover(const QString &root,
                                     const LibraryBatchImportOptions &options = {});
    bool startSelected(const QString &batchId, const QStringList &directories);
    LibraryImportCancelResult cancel();
    bool shutdown();
    bool isActive() const { return m_active || m_finalizing; }
    bool isShuttingDown() const { return m_shuttingDown; }
    QString currentTaskId() const { return m_childTaskId; }
    QString batchId() const { return m_result.batchId; }
    LibraryBatchImportState state() const { return m_state; }

signals:
    void previewReady(QString batchId, asmr::WorkDirectoryDiscoveryResult result);
    void stateChanged(QString batchId, asmr::LibraryBatchImportState state, QString text);
    // Runs in the coordinator's owner thread, after submit returned its ID.
    void childStarted(QString batchId, asmr::LibraryImportSubmission child,
                      QString directory, int index, int total);
    void finished(asmr::LibraryBatchImportResult result);
    void shutdownFinished();

private:
    friend class LibraryBatchImportServiceTestAccess;
    void scheduleNext();
    void startNext(const QString &batchId);
    void consumeChild(const LibraryImportTaskResult &result);
    void finishBatch();
    void finishShutdownIfReady();
    void singleDestroyed();
    void publishChildStage(const QString &text);
    void publishState(LibraryBatchImportState state, const QString &text);

    QString m_databasePath;
    QPointer<LibraryImportService> m_single;
    LibraryBatchImportOptions m_options;
    LibraryBatchImportState m_state = LibraryBatchImportState::Idle;
    WorkDirectoryDiscoveryResult m_preview;
    LibraryBatchImportResult m_result;
    QVector<qsizetype> m_queue;
    qsizetype m_next = 0;
    qsizetype m_currentItem = -1;
    QString m_childTaskId;
    QFuture<WorkDirectoryDiscoveryResult> m_discoveryLifetime;
    std::shared_ptr<std::atomic<bool>> m_cancelDiscovery;
    // Deterministic private seam; executed on discovery worker only.
    std::shared_ptr<const std::function<void()>> m_beforeDiscovery;
    bool m_discovering = false;
    bool m_active = false;
    bool m_finalizing = false;
    bool m_submitting = false;
    std::optional<LibraryImportTaskResult> m_deferredChildResult;
    bool m_stopRequested = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::WorkDirectoryDiscoveryResult)
Q_DECLARE_METATYPE(asmr::LibraryBatchImportState)
Q_DECLARE_METATYPE(asmr::LibraryBatchImportResult)
