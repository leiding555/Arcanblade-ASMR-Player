#include "ui/MainWindow.h"

#include "ui/BatchImportPreviewDialog.h"
#include "ui/LibraryPanel.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QStringList>

namespace asmr {
namespace {
QString outcomeName(LibraryBatchItemOutcome outcome)
{
    switch (outcome) {
    case LibraryBatchItemOutcome::Imported: return QStringLiteral("已导入");
    case LibraryBatchItemOutcome::ImportedAssociationFailed: return QStringLiteral("已导入，字幕关联异常");
    case LibraryBatchItemOutcome::Skipped: return QStringLiteral("跳过");
    case LibraryBatchItemOutcome::Failed: return QStringLiteral("失败");
    case LibraryBatchItemOutcome::Cancelled: return QStringLiteral("已取消");
    case LibraryBatchItemOutcome::NotStarted: return QStringLiteral("未开始");
    }
    return {};
}
}

void MainWindow::connectBatchImport()
{
    // Register this AFTER the original queued child-result handlers. They
    // consume a finished child's UI state before the coordinator advances.
    m_libraryBatchImportService = new LibraryBatchImportService(
        m_libraryDatabasePath, m_libraryImportService, this);
    connect(m_libraryPanel, &LibraryPanel::importRootDirectoryRequested,
            this, &MainWindow::openLibraryRootDirectory);
    connect(m_libraryBatchImportService, &LibraryBatchImportService::previewReady,
            this, &MainWindow::applyBatchPreview);
    connect(m_libraryBatchImportService, &LibraryBatchImportService::childStarted, this,
            [this](const QString &batchId, const LibraryImportSubmission &child,
                   const QString &, int, int) {
        if (m_closing || batchId != m_libraryBatchImportService->batchId())
            return;
        m_libraryTaskId = child.taskId;
        m_libraryBatchChildTaskId = child.taskId;
        m_libraryScanSummary.clear();
        m_libraryImportState = LibraryImportState::Scanning;
    });
    connect(m_libraryBatchImportService, &LibraryBatchImportService::stateChanged, this,
            [this](const QString &batchId, LibraryBatchImportState state, const QString &text) {
        if (m_closing || batchId != m_libraryBatchImportService->batchId()
            || state == LibraryBatchImportState::Finished)
            return;
        const auto displayed = state == LibraryBatchImportState::Stopping
            ? LibraryImportState::Cancelling
            : m_libraryImportState == LibraryImportState::Committing
                    && state == LibraryBatchImportState::Importing
                ? LibraryImportState::Committing : LibraryImportState::Scanning;
        m_libraryPanel->setImportState(displayed, text, m_libraryBatchImportService->isActive(), true);
        updateScanRootManager();
    });
    connect(m_libraryBatchImportService, &LibraryBatchImportService::finished,
            this, &MainWindow::applyBatchFinished);
    connect(m_libraryBatchImportService, &LibraryBatchImportService::shutdownFinished, this, [this] {
        m_batchShutdownFinished = true;
        finishCloseIfReady();
    });
}

void MainWindow::openLibraryRootDirectory()
{
    if (m_closing)
        return;
    if (libraryScanBusy()) {
        m_libraryPanel->setImportError(QStringLiteral("已有导入任务，请等待完成或取消。"));
        return;
    }
    const QString root = QFileDialog::getExistingDirectory(
        this, QStringLiteral("导入作品总目录（一级子目录分别作为作品）"),
        QString(), QFileDialog::ShowDirsOnly);
    if (!root.isEmpty())
        submitLibraryBatchDiscovery(root);
}

void MainWindow::submitLibraryBatchDiscovery(const QString &directory,
                                             const LibraryBatchImportOptions &options)
{
    if (m_closing)
        return;
    if (!m_libraryDatabaseReady) {
        m_libraryPanel->setImportError(QStringLiteral("作品库尚未成功打开。"));
        return;
    }
    if (libraryScanBusy()) {
        m_libraryPanel->setImportError(QStringLiteral("已有导入任务，请等待完成或取消。"));
        return;
    }
    m_libraryImportState = LibraryImportState::Scanning;
    m_libraryPanel->setImportFeedback({});
    const auto accepted = m_libraryBatchImportService->discover(directory, options);
    if (accepted.status != LibraryImportSubmissionStatus::Accepted)
        m_libraryPanel->setImportError(QStringLiteral("当前无法开始总目录导入。"));
}

void MainWindow::applyBatchPreview(const QString &batchId,
                                  const WorkDirectoryDiscoveryResult &result)
{
    if (m_closing || batchId != m_libraryBatchImportService->batchId()
        || m_libraryBatchImportService->state() != LibraryBatchImportState::AwaitingSelection)
        return;
    if (!m_batchImportPreviewDialog) {
        m_batchImportPreviewDialog = new BatchImportPreviewDialog(this);
        connect(m_batchImportPreviewDialog, &QDialog::accepted, this, [this] {
            if (m_closing || m_batchPreviewId != m_libraryBatchImportService->batchId())
                return;
            if (!m_libraryBatchImportService->startSelected(
                    m_batchPreviewId, m_batchImportPreviewDialog->selectedDirectories())) {
                m_libraryPanel->setImportError(QStringLiteral("候选已失效或未选择作品，请重新导入总目录。"));
                m_libraryBatchImportService->cancel();
            }
        });
        connect(m_batchImportPreviewDialog, &QDialog::rejected, this, [this] {
            if (!m_closing && m_batchPreviewId == m_libraryBatchImportService->batchId()
                && m_libraryBatchImportService->state() == LibraryBatchImportState::AwaitingSelection)
                m_libraryBatchImportService->cancel();
        });
    }
    m_batchPreviewId = batchId;
    m_batchImportPreviewDialog->setDiscovery(result);
    m_batchImportPreviewDialog->show();
    m_batchImportPreviewDialog->raise();
    m_batchImportPreviewDialog->activateWindow();
}

void MainWindow::applyBatchFinished(const LibraryBatchImportResult &result)
{
    if (result.batchId != m_libraryBatchImportService->batchId())
        return;
    m_batchPreviewId.clear();
    if (m_batchImportPreviewDialog)
        m_batchImportPreviewDialog->hide();
    if (m_closing)
        return;
    QString text = result.cancelled ? QStringLiteral("批量导入已停止。")
        : result.errorMessage.isEmpty() ? QStringLiteral("批量导入完成。")
                                       : QStringLiteral("总目录导入失败：%1").arg(result.errorMessage);
    text += QStringLiteral("\n候选 %1 部，选择 %2 部；已导入 %3（其中字幕关联异常 %4），跳过 %5，失败 %6，取消 %7，未开始 %8。")
        .arg(result.items.size()).arg(result.selectedCount).arg(result.imported).arg(result.associationFailed)
        .arg(result.skipped).arg(result.failed).arg(result.cancelledItems).arg(result.notStarted);
    text += QStringLiteral("\n总目录直属文件未纳入 %1 个，链接或 junction 跳过 %2 个。")
        .arg(result.rootFilesSkipped).arg(result.linksSkipped);
    QStringList details = result.diagnostics;
    if (!result.errorMessage.isEmpty())
        details.prepend(result.errorMessage);
    if (result.suppressedDiagnostics)
        details.append(QStringLiteral("另有 %1 条总目录诊断未显示。").arg(result.suppressedDiagnostics));
    constexpr qsizetype maximumDisplayedItems = 200;
    const qsizetype displayed = qMin(result.items.size(), maximumDisplayedItems);
    for (qsizetype index = 0; index < displayed; ++index) {
        const auto &item = result.items[index];
        details.append(QStringLiteral("%1 — %2\n%3")
            .arg(item.directory, outcomeName(item.outcome), item.errorMessage));
        details.append(item.diagnostics);
        if (item.suppressedDiagnostics)
            details.append(QStringLiteral("另有 %1 条作品诊断未显示。").arg(item.suppressedDiagnostics));
    }
    if (result.items.size() > displayed)
        details.append(QStringLiteral("另有 %1 部作品的逐项详情未显示，汇总计数包含全部作品。")
                           .arg(result.items.size() - displayed));
    m_libraryPanel->setImportFeedback({}, details.join(QLatin1Char('\n')));
    m_libraryPanel->setImportState(result.cancelled ? LibraryImportState::Cancelled
        : result.errorMessage.isEmpty() && !result.failed && !result.associationFailed
            ? LibraryImportState::Completed : LibraryImportState::Failed,
        text, false);
    if (result.cancelled && (result.failed || result.associationFailed || !result.errorMessage.isEmpty()))
        m_libraryPanel->setImportError(text + (result.errorMessage.isEmpty()
            ? QString() : QLatin1Char('\n') + result.errorMessage));
}
} // namespace asmr
