#pragma once

#include "library/LibraryImportService.h"
#include "library/WorkDirectoryDiscovery.h"

#include <QFuture>
#include <QPointer>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>

namespace asmr {

enum class LibraryRootRefreshItemOutcome { Added, Updated, Removed, Skipped, Overlap, Failed, Cancelled, NotStarted };
struct LibraryRootRefreshOptions {
    WorkDirectoryDiscoveryOptions discovery;
    DirectoryScanOptions scan;
    qsizetype maximumItemDiagnostics = 20;
};
struct LibraryRootRefreshItemResult {
    QString directory;
    qint64 workId = 0;
    LibraryRootRefreshItemOutcome outcome = LibraryRootRefreshItemOutcome::NotStarted;
    bool requestedRemoval = false;
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
struct LibraryRootRefreshRootResult {
    QString rootDirectory;
    bool success = false;
    bool cancelled = false;
    QString errorMessage;
    QStringList diagnostics;
    qsizetype suppressedDiagnostics = 0;
    qsizetype rootFilesSkipped = 0;
    qsizetype linksSkipped = 0;
    qsizetype discovered = 0;
    qsizetype imported = 0;
    qsizetype updated = 0;
    qsizetype removed = 0;
    qsizetype skipped = 0;
    qsizetype failed = 0;
    qsizetype associationFailed = 0;
    qsizetype notStarted = 0;
    QStringList overlapDirectories;
    QVector<LibraryRootRefreshItemResult> items;
};
struct LibraryRootRefreshResult {
    QString taskId;
    bool cancelled = false;
    QString errorMessage;
    QVector<LibraryRootRefreshRootResult> roots;
};
class LibraryRootRefreshServiceTestAccess;

// Owner-thread coordinator borrowing the application's single writer. Root
// discovery runs off-thread; roots and accepted work tasks run sequentially.
class LibraryRootRefreshService final : public QObject {
    Q_OBJECT
public:
    explicit LibraryRootRefreshService(QString databasePath, LibraryImportService *single,
                                       QObject *parent = nullptr);
    ~LibraryRootRefreshService() override;
    LibraryImportSubmission start(const QStringList &roots,
                                  const LibraryRootRefreshOptions &options = {});
    LibraryImportCancelResult cancel();
    bool shutdown();
    bool isActive() const { return m_active || m_finalizing; }
    bool isShuttingDown() const { return m_shuttingDown; }
    QString currentTaskId() const { return m_childTaskId; }
    QString taskId() const { return m_result.taskId; }

signals:
    void stateChanged(QString text);
    void childStarted(asmr::LibraryImportSubmission submission, QString directory, int index, int total);
    void finished(asmr::LibraryRootRefreshResult result);
    void shutdownFinished();

private:
    friend class LibraryRootRefreshServiceTestAccess;
    struct Preparation {
        WorkDirectoryDiscoveryResult discovery;
        QVector<LibraryWork> missingWorks;
    };
    static Preparation prepare(const QString &databasePath, const QString &root,
        const LibraryRootRefreshOptions &options, const std::shared_ptr<std::atomic<bool>> &cancelled,
        const std::shared_ptr<const std::function<void()>> &beforeDiscovery);
    void scheduleNext();
    void advance(const QString &id);
    void beginRoot();
    void consumeChild(const LibraryImportTaskResult &child);
    void finishRoot();
    void finishRefresh();
    void finishShutdownIfReady();
    void singleDestroyed();
    void publishChildStage(const QString &text);

    QString m_databasePath;
    QPointer<LibraryImportService> m_single;
    LibraryRootRefreshOptions m_options;
    LibraryRootRefreshResult m_result;
    qsizetype m_rootIndex = 0;
    QVector<qsizetype> m_queue;
    qsizetype m_next = 0;
    qsizetype m_currentItem = -1;
    QString m_childTaskId;
    QFuture<Preparation> m_discoveryLifetime;
    std::shared_ptr<std::atomic<bool>> m_cancelDiscovery;
    std::shared_ptr<const std::function<void()>> m_beforeDiscovery;
    std::optional<LibraryImportTaskResult> m_deferredChildResult;
    bool m_rootPrepared = false;
    bool m_discovering = false;
    bool m_active = false;
    bool m_finalizing = false;
    bool m_submitting = false;
    bool m_stopRequested = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::LibraryRootRefreshResult)
