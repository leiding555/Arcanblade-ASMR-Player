#pragma once

#include "library/LibraryRepository.h"
#include "library/WorkReferenceTypes.h"

#include <QDialog>
#include <QFuture>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QLabel;
class QAction;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace asmr {
class WorkReferenceDialogTests;

// Read-only view. The owner routes updates through the shared import service.
class WorkReferenceDialog final : public QDialog {
    Q_OBJECT
public:
    explicit WorkReferenceDialog(QString databasePath, LibraryWork work, QWidget *parent = nullptr);
    ~WorkReferenceDialog() override;
    const LibraryWork &work() const { return m_work; }
    void setWork(const LibraryWork &work);
    void refresh();
    void setBusy(bool busy);
    void setFeedback(const QString &message);
    void setUpdateFeedback(const QString &message);
    void invalidate();
    void shutdown();
    bool isIdle() const { return !m_active && !m_pending; }
    bool isShuttingDown() const { return m_shuttingDown; }

signals:
    void updateRequested(bool forceReextract);
    void shutdownFinished();

protected:
    void done(int result) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    friend class WorkReferenceDialogTests;
    enum class JobKind { Load, Preview, OpenDirectory };
    struct Job {
        quint64 generation = 0;
        JobKind kind = JobKind::Load;
        LibraryWork work;
        WorkReferenceRecord record;
        quint64 openSession = 0;
    };
    struct JobResult {
        WorkReferenceSnapshot snapshot;
        QStringList pages;
        QString directory, error;
    };
    static QStringList renderPreview(const WorkReferenceRecord &record, const std::atomic_bool &cancelled);
    static QString checkedDirectory(const LibraryWork &work, const WorkReferenceRecord &record);
    void request(Job job);
    void start(Job job);
    void applySnapshot(WorkReferenceSnapshot snapshot);
    void selectRecord();
    void showPage();
    void updateControls();
    void invalidateContextMenu();
    void openContextDirectory();
    void finishShutdownIfReady();

    QString m_databasePath;
    QString m_feedbackText, m_updateFeedback;
    LibraryWork m_work;
    WorkReferenceSnapshot m_snapshot;
    QStringList m_pages;
    int m_page = 0;
    bool m_active = false, m_loading = false, m_busy = false;
    bool m_invalid = false, m_shuttingDown = false, m_shutdownNotified = false;
    quint64 m_generation = 0;
    quint64 m_openSession = 0, m_menuRevision = 0;
    std::optional<Job> m_pending;
    std::optional<Job> m_menuTarget;
    std::optional<int> m_requestedResult;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFuture<JobResult> m_lifetime;
    std::shared_ptr<const std::function<WorkReferenceSnapshot(const LibraryWork &)>> m_loadTestHook;
    std::shared_ptr<const std::function<QString(const LibraryWork &, const WorkReferenceRecord &)>> m_directoryTestHook;
    QLabel *m_workLabel = nullptr, *m_feedbackLabel = nullptr, *m_pageLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QPushButton *m_update = nullptr;
    QPushButton *m_previous = nullptr, *m_next = nullptr;
    QMenu *m_contextMenu = nullptr;
    QAction *m_openDirectoryAction = nullptr;
};
} // namespace asmr
