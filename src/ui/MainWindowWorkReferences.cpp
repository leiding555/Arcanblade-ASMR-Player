#include "ui/MainWindow.h"
#include "ui/LibraryPanel.h"
#include "ui/WorkReferenceDialog.h"

#include <QTimer>
#include <QStringList>
#include <algorithm>
#include <utility>

namespace asmr {
void MainWindow::connectWorkReferences()
{
    connect(m_libraryPanel, &LibraryPanel::workReferencesRequested, this, &MainWindow::showWorkReferences);
    connect(m_libraryImportService, &LibraryImportService::stateChanged, this,
        [this](const QString &, LibraryImportState state) {
            if (!m_workReferenceDialog || m_closing) return;
            m_workReferenceDialog->setBusy(libraryScanBusy());
            if (state == LibraryImportState::ProcessingReferences)
                m_workReferenceDialog->setFeedback(QStringLiteral("正在提取和缓存参考资料…"));
        }, Qt::QueuedConnection);
    connect(m_libraryImportService, &LibraryImportService::importCommitted, this,
        [this](const QString &, const LibraryImportResult &result) {
            if (m_workReferenceDialog && result.success && result.removed
                && m_workReferenceDialog->work().id == result.workId)
                m_workReferenceDialog->invalidate();
        }, Qt::QueuedConnection);
    connect(m_libraryImportService, &LibraryImportService::taskFinished, this,
        [this](const LibraryImportTaskResult &result) {
            const bool requestedUpdate = result.taskId == m_referenceUpdateTaskId;
            const qint64 requestedWork = m_referenceUpdateWorkId;
            if (requestedUpdate) { m_referenceUpdateTaskId.clear(); m_referenceUpdateWorkId = 0; }
            if (!m_workReferenceDialog || m_closing) return;
            m_workReferenceDialog->setBusy(libraryScanBusy());
            if (requestedUpdate && requestedWork == m_workReferenceDialog->work().id && !result.databaseCommitted) {
                m_workReferenceDialog->setUpdateFeedback(QStringLiteral("资料更新未执行：") + result.errorMessage);
                m_workReferenceDialog->refresh();
                return;
            }
            if (result.repositoryResult.workId != m_workReferenceDialog->work().id) return;
            if (result.repositoryResult.removed) m_workReferenceDialog->invalidate();
            else if (result.databaseCommitted) {
                QStringList feedback;
                if (!result.references.error.isEmpty())
                    feedback.append(QStringLiteral("作品已保留；资料同步未完成：") + result.references.error);
                if (result.references.failedCount)
                    feedback.append(QStringLiteral("%1 份资料提取失败，请查看对应条目。").arg(result.references.failedCount));
                feedback.append(result.references.warnings);
                m_workReferenceDialog->setUpdateFeedback(feedback.join(QLatin1Char('\n')));
                m_workReferenceDialog->refresh();
            }
        }, Qt::QueuedConnection);
    const auto updateAvailability = [this] {
        if (m_workReferenceDialog && !m_closing) m_workReferenceDialog->setBusy(libraryScanBusy());
    };
    connect(m_libraryBatchImportService, &LibraryBatchImportService::finished, this,
        [updateAvailability](const LibraryBatchImportResult &) { updateAvailability(); }, Qt::QueuedConnection);
    connect(m_libraryRootRefreshService, &LibraryRootRefreshService::finished, this,
        [updateAvailability](const LibraryRootRefreshResult &) { updateAvailability(); }, Qt::QueuedConnection);
}

void MainWindow::showWorkReferences(qint64 workId)
{
    if (m_closing || !m_libraryDatabaseReady || workId <= 0
        || m_removedLibraryWorkIds.contains(workId)) return;
    const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const LibraryWork &work) { return work.id == workId; });
    if (found == m_libraryWorksSnapshot.cend()) return;
    if (m_workReferenceDialog) {
        if (m_workReferenceDialog->isShuttingDown()) {
            m_pendingReferenceWorkId = workId;
            return;
        }
        if (m_workReferenceDialog->work().id != workId
            || m_workReferenceDialog->work().rootPath != found->rootPath
            || m_workReferenceDialog->work().importedAtMs != found->importedAtMs)
            m_workReferenceDialog->setWork(*found);
        else m_workReferenceDialog->refresh();
        m_workReferenceDialog->setBusy(libraryScanBusy());
        m_workReferenceDialog->show(); m_workReferenceDialog->raise();
        m_workReferenceDialog->activateWindow();
        return;
    }
    auto *dialog = new WorkReferenceDialog(m_libraryDatabasePath, *found, this);
    m_workReferenceDialog = dialog;
    dialog->setBusy(libraryScanBusy());
    connect(dialog, &WorkReferenceDialog::updateRequested, this, &MainWindow::updateWorkReferences);
    connect(dialog, &WorkReferenceDialog::shutdownFinished, this, [this] { finishCloseIfReady(); });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_workReferenceDialog != dialog) return;
        m_workReferenceDialog = nullptr; dialog->deleteLater();
        finishCloseIfReady();
        const auto pending = std::exchange(m_pendingReferenceWorkId, qint64(0));
        if (!m_closing && pending > 0)
            QTimer::singleShot(0, this, [this, pending] { showWorkReferences(pending); });
    });
    dialog->show();
}

void MainWindow::updateWorkReferences(bool forceReextract)
{
    if (m_closing || !m_workReferenceDialog || !m_libraryDatabaseReady) return;
    if (libraryScanBusy()) {
        m_workReferenceDialog->setFeedback(QStringLiteral("已有导入或更新任务，请等待完成。"));
        m_workReferenceDialog->setBusy(true);
        return;
    }
    const auto expected = m_workReferenceDialog->work();
    const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [&expected](const LibraryWork &work) {
            return work.id == expected.id && work.rootPath == expected.rootPath
                && work.importedAtMs == expected.importedAtMs;
        });
    if (found == m_libraryWorksSnapshot.cend() || m_removedLibraryWorkIds.contains(expected.id)) {
        m_workReferenceDialog->invalidate(); return;
    }
    const auto submission = m_libraryImportService->submitRefresh(expected.id, expected.rootPath, {},
        forceReextract ? WorkReferenceRefreshPolicy::ForceReextract : WorkReferenceRefreshPolicy::ReuseCache);
    if (submission.status != LibraryImportSubmissionStatus::Accepted) {
        m_workReferenceDialog->setFeedback(QStringLiteral("当前无法提交资料更新，请稍后重试。")); return;
    }
    m_libraryTaskId = submission.taskId;
    m_referenceUpdateTaskId = submission.taskId;
    m_referenceUpdateWorkId = expected.id;
    m_libraryImportState = LibraryImportState::Scanning;
    m_libraryScanSummary.clear();
    m_libraryPanel->setImportFeedback({});
    m_libraryPanel->setImportState(m_libraryImportState, QStringLiteral("正在更新作品及参考资料…"), true);
    m_workReferenceDialog->setBusy(true);
    m_workReferenceDialog->setUpdateFeedback({});
    m_workReferenceDialog->setFeedback(QStringLiteral("正在更新作品及参考资料…"));
    updateScanRootManager();
}
} // namespace asmr
