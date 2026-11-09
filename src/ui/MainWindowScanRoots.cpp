#include "ui/MainWindow.h"

#include "library/ScanRootStore.h"
#include "playback/AudioPlayer.h"
#include "ui/PlaybackControlsWidget.h"
#include "ui/SubtitlePanel.h"
#include "ui/LibraryPanel.h"
#include "ui/ScanRootManagerDialog.h"
#include "ui/WorkReferenceDialog.h"
#include "ui/PlayerMessageBox.h"

#include <QFileDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>

#include <algorithm>
#include <filesystem>

namespace asmr {

bool MainWindow::libraryScanBusy() const
{
    return m_scanRootSettingsInProgress || !m_libraryTaskId.isEmpty()
        || (m_libraryImportService && m_libraryImportService->isBusy())
        || (m_libraryBatchImportService && m_libraryBatchImportService->isActive())
        || (m_libraryRootRefreshService && m_libraryRootRefreshService->isActive());
}

void MainWindow::connectScanRoots()
{
    m_scanRootStore = std::make_unique<ScanRootStore>(m_libraryDatabasePath, nullptr, false);
    m_libraryRootRefreshService = new LibraryRootRefreshService(
        m_libraryDatabasePath, m_libraryImportService, this);
    connect(m_libraryPanel, &LibraryPanel::manageScanRootsRequested,
            this, &MainWindow::showScanRootManager);
    connect(m_libraryPanel, &LibraryPanel::refreshScanRootsRequested, this, [this] {
        startRootRefresh(m_scanRootStore->roots());
    });
    connect(m_libraryRootRefreshService, &LibraryRootRefreshService::childStarted, this,
        [this](const LibraryImportSubmission &child, const QString &, int, int) {
        if (m_closing || !m_libraryRootRefreshService->isActive())
            return;
        m_libraryTaskId = child.taskId;
        m_libraryRootChildTaskId = child.taskId;
        m_libraryScanSummary.clear();
        m_libraryImportState = LibraryImportState::Scanning;
    });
    connect(m_libraryRootRefreshService, &LibraryRootRefreshService::stateChanged, this,
        [this](const QString &text) {
        if (m_closing)
            return;
        m_libraryPanel->setImportState(m_libraryImportState, text,
                                       m_libraryRootRefreshService->isActive(), true);
        updateScanRootManager();
    });
    connect(m_libraryRootRefreshService, &LibraryRootRefreshService::finished,
            this, &MainWindow::applyRootRefreshFinished);
    connect(m_libraryRootRefreshService, &LibraryRootRefreshService::shutdownFinished, this, [this] {
        m_rootShutdownFinished = true;
        finishCloseIfReady();
    });
    // Start once the existing operation and its coordinator have both unwound.
    const auto afterTask = [this] {
        QTimer::singleShot(0, this, [this] {
            updateScanRootManager();
            tryStartScanRootInspection();
            tryStartAiSubtitleRefresh();
            tryStartStartupRootRefresh();
        });
    };
    connect(m_libraryImportService, &LibraryImportService::taskFinished, this, afterTask);
    connect(m_libraryBatchImportService, &LibraryBatchImportService::finished, this, afterTask);
    connect(m_libraryRootRefreshService, &LibraryRootRefreshService::finished, this, afterTask);
    startScanRootSettingsChange(0);
}

bool MainWindow::startScanRootSettingsChange(int kind, const QString &root, bool enabled)
{
    if (m_closing || m_scanRootSettingsInProgress) return false;
    m_scanRootSettingsInProgress = true;
    const auto databasePath = m_libraryDatabasePath;
    const auto session = m_scanRootManagerSession;
    const auto hook = m_scanRootSettingsTestHook;
    auto promise = std::make_shared<QPromise<ScanRootSettingsResult>>();
    auto *watcher = new QFutureWatcher<ScanRootSettingsResult>(this);
    connect(watcher, &QFutureWatcher<ScanRootSettingsResult>::finished, this,
        [this, watcher, kind, session] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_scanRootSettingsInProgress = false;
        if (result.success) {
            m_scanRootStore->adoptSnapshot(result.snapshot);
            m_scanRootSettingsReady = true;
            QSet<QString> retained;
            for (const auto &root : result.snapshot.roots) retained.insert(ScanRootStore::pathKey(root));
            for (auto it = m_scanRootStatuses.begin(); it != m_scanRootStatuses.end();)
                if (!retained.contains(it.key())) it = m_scanRootStatuses.erase(it); else ++it;
            for (auto it = m_scanRootPresentations.begin(); it != m_scanRootPresentations.end();)
                if (!retained.contains(it.key())) it = m_scanRootPresentations.erase(it); else ++it;
        } else if (!m_closing && (kind != 5 || (session == m_scanRootManagerSession
                    && m_scanRootManagerDialog && m_scanRootManagerDialog->isVisible()))) {
            m_libraryPanel->setImportError(result.errorMessage);
        }
        updateScanRootManager();
        if (result.success && !m_closing) {
            if (kind == 1) startRootRefresh({result.root});
            else if (kind == 2) {
                m_scanRootStatuses.remove(ScanRootStore::pathKey(result.root));
                m_scanRootPresentations.remove(ScanRootStore::pathKey(result.root));
                m_libraryPanel->setImportFeedback(QStringLiteral(
                    "已移除扫描根目录；作品、媒体文件、字幕绑定、首选和播放进度均已保留。"));
            } else if (kind == 5 && session == m_scanRootManagerSession
                       && m_scanRootManagerDialog && m_scanRootManagerDialog->isVisible()) {
                if (!result.directory.isEmpty()
                    && !QDesktopServices::openUrl(QUrl::fromLocalFile(result.directory)))
                    m_libraryPanel->setImportError(QStringLiteral("无法打开扫描目录。"));
            }
            scheduleStartupRootRefresh();
            tryStartStartupRootRefresh();
        }
        if (kind == 5) m_scanRootInspectionPending = true;
        tryStartScanRootInspection();
        finishCloseIfReady();
    });
    m_scanRootSettingsLifetime = promise->future();
    watcher->setFuture(m_scanRootSettingsLifetime);
    QThreadPool::globalInstance()->start([promise, databasePath, kind, root, enabled, hook] {
        promise->start();
        ScanRootSettingsResult result;
        try {
            if (hook) (*hook)(ScanRootSettingsTestPoint::BeforeTask, kind);
            ScanRootStore store(databasePath, nullptr, false);
            if (kind == 0 || kind == 4 || kind == 5) result.success = store.reload();
            else if (kind == 3) result.success = store.reload() && store.setStartupEnabled(enabled);
            else {
                const auto change = kind == 1 ? store.addRoot(root) : store.removeRoot(root);
                result.success = change.success; result.root = change.root; result.errorMessage = change.errorMessage;
            }
            if (result.success && (kind == 0 || kind == 4)) {
                const auto cleaned = store.pruneMissingRoots();
                result.success = cleaned.success;
                result.errorMessage = cleaned.errorMessage;
            }
            if (result.success && kind == 5) {
                result.root = ScanRootStore::normalizePath(root);
                const auto key = ScanRootStore::pathKey(result.root);
                const auto roots = store.roots();
                const auto registered = std::any_of(roots.cbegin(), roots.cend(),
                    [&](const auto &path) { return ScanRootStore::pathKey(path) == key; });
                const QFileInfo info(result.root);
                // Directory I/O stays on this worker; UI receives only a checked canonical path.
                if (!registered || result.root.isEmpty() || info.isSymbolicLink() || info.isJunction()
                    || !info.isDir() || !info.isReadable() || info.canonicalFilePath().isEmpty()) {
                    result.success = false;
                    result.errorMessage = QStringLiteral("扫描目录已不可用或配置已变化，无法打开。");
                } else {
                    std::error_code error;
#ifdef Q_OS_WIN
                    const auto path = std::filesystem::path(result.root.toStdWString());
#else
                    const auto path = std::filesystem::path(QFile::encodeName(result.root).constData());
#endif
                    const std::filesystem::directory_iterator entries(path, error);
                    if (error) {
                        result.success = false;
                        result.errorMessage = QStringLiteral("无法访问扫描目录，未打开。");
                    } else result.directory = info.canonicalFilePath();
                }
            }
            result.snapshot = store.snapshot();
            if (result.errorMessage.isEmpty()) result.errorMessage = result.snapshot.errorMessage;
            if (!result.success && result.errorMessage.isEmpty())
                result.errorMessage = QStringLiteral("无法保存扫描目录设置。");
            if (hook) (*hook)(ScanRootSettingsTestPoint::AfterTask, kind);
        } catch (...) {
            result.success = false;
            result.directory.clear();
            result.errorMessage = QStringLiteral("扫描目录设置处理失败。");
        }
        promise->addResult(std::move(result)); promise->finish();
    });
    updateScanRootManager();
    return true;
}

void MainWindow::showScanRootManager()
{
    if (m_closing)
        return;
    if (!m_scanRootManagerDialog) {
        m_scanRootManagerDialog = new ScanRootManagerDialog(this);
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::addRootRequested, this, [this] {
            if (m_closing || libraryScanBusy())
                return;
            const auto directory = QFileDialog::getExistingDirectory(
                this, QStringLiteral("添加扫描总目录（一级子目录分别作为作品）"),
                {}, QFileDialog::ShowDirsOnly);
            if (!directory.isEmpty())
                addScanRoot(directory);
        });
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::removeRootRequested,
                this, &MainWindow::requestScanRootRemoval);
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::hidden, this, [this] {
            if (m_scanRootRemovalConfirmation) m_scanRootRemovalConfirmation->reject();
        });
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::refreshRootRequested,
                this, [this](const QString &root) { startRootRefresh({root}); });
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::refreshAllRequested,
                this, [this] { startRootRefresh(m_scanRootStore->roots()); });
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::startupScanChanged,
                this, [this](bool enabled) {
            if (m_closing || libraryScanBusy())
                return;
            startScanRootSettingsChange(3, {}, enabled);
        });
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::reviewOverlapsRequested,
                this, [this](const QString &root) {
            if (!m_closing && !libraryScanBusy())
                submitLibraryBatchDiscovery(root);
        });
        connect(m_scanRootManagerDialog, &ScanRootManagerDialog::openRootRequested, this,
            [this](const QString &root) {
                if (!m_closing && !libraryScanBusy())
                    startScanRootSettingsChange(5, root);
            });
    }
    if (m_scanRootRemovalConfirmation) m_scanRootRemovalConfirmation->reject();
    ++m_scanRootManagerSession;
    updateScanRootManager();
    m_scanRootManagerDialog->show();
    m_scanRootManagerDialog->raise();
    m_scanRootManagerDialog->activateWindow();
    m_scanRootInspectionPending = true;
    tryStartScanRootInspection();
}

void MainWindow::tryStartScanRootInspection()
{
    if (!m_scanRootInspectionPending || m_closing || !m_scanRootSettingsReady || libraryScanBusy()) return;
    m_scanRootInspectionPending = false;
    startScanRootSettingsChange(4);
}

bool MainWindow::addScanRoot(const QString &directory)
{
    if (m_closing || libraryScanBusy() || !m_scanRootSettingsReady) return false;
    const auto path = ScanRootStore::normalizePath(directory);
    if (path.isEmpty()) return false;
    const auto key = ScanRootStore::pathKey(path);
    for (const auto &existing : m_scanRootStore->roots()) {
        const auto current = ScanRootStore::pathKey(existing);
        if (key == current || key.startsWith(current + u'/') || current.startsWith(key + u'/')) {
            m_libraryPanel->setImportError(QStringLiteral("扫描目录已经存在或互相嵌套。"));
            return false;
        }
    }
    return startScanRootSettingsChange(1, path);
}
bool MainWindow::removeScanRoot(const QString &directory)
{
    if (m_closing || libraryScanBusy() || !m_scanRootSettingsReady) return false;
    return startScanRootSettingsChange(2, directory);
}
void MainWindow::requestScanRootRemoval(const QString &directory)
{
    if (m_closing || libraryScanBusy() || !m_scanRootSettingsReady
        || !m_scanRootManagerDialog || !m_scanRootManagerDialog->isVisible()) return;
    if (m_scanRootRemovalConfirmation) {
        m_scanRootRemovalConfirmation->raise();
        m_scanRootRemovalConfirmation->activateWindow();
        return;
    }
    const auto root = ScanRootStore::normalizePath(directory);
    const auto key = ScanRootStore::pathKey(root);
    const auto registered = [this, &key] {
        const auto roots = m_scanRootStore->roots();
        return !key.isEmpty() && std::any_of(roots.cbegin(), roots.cend(), [&key](const auto &entry) {
            return ScanRootStore::pathKey(entry) == key;
        });
    };
    if (!registered()) return;
    const auto session = m_scanRootManagerSession;
    PlayerMessageBox dialog(QMessageBox::Warning, QStringLiteral("移除扫描目录"),
        QStringLiteral("移除以下扫描根目录？\n%1\n\n"
            "将停止扫描此目录。已入库作品、字幕绑定、默认设置、播放进度和本地文件均保留。")
            .arg(root), QMessageBox::NoButton, m_scanRootManagerDialog);
    dialog.setObjectName(QStringLiteral("scanRootRemovalConfirmation"));
    dialog.setTextFormat(Qt::PlainText);
    auto *remove = dialog.addButton(QStringLiteral("移除"), QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("confirmScanRootRemovalButton"));
    auto *cancel = dialog.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    cancel->setObjectName(QStringLiteral("cancelScanRootRemovalButton"));
    dialog.setDefaultButton(cancel);
    dialog.setEscapeButton(cancel);
    m_scanRootRemovalConfirmation = &dialog;
    const auto reset = qScopeGuard([this] { m_scanRootRemovalConfirmation = nullptr; });
    dialog.exec();
    if (dialog.clickedButton() != remove || m_closing || libraryScanBusy() || !m_scanRootSettingsReady
        || session != m_scanRootManagerSession || !m_scanRootManagerDialog->isVisible() || !registered()) return;
    removeScanRoot(root);
}
void MainWindow::updateScanRootManager()
{
    if (m_workReferenceDialog) m_workReferenceDialog->setBusy(libraryScanBusy());
    if (!m_scanRootManagerDialog || !m_scanRootStore)
        return;
    m_scanRootManagerDialog->setConfiguration(m_scanRootStore->roots(),
                                               m_scanRootStore->startupEnabled());
    m_scanRootManagerDialog->setBusy(m_closing || !m_libraryDatabaseReady || !m_scanRootSettingsReady || libraryScanBusy());
    for (const auto &root : m_scanRootStore->roots()) {
        const auto key = ScanRootStore::pathKey(root);
        const auto presentation = m_scanRootPresentations.value(key,
            ScanRootPresentation{QStringLiteral("尚未扫描"), false});
        m_scanRootManagerDialog->setRootStatus(root,
            m_scanRootStatuses.value(key, QStringLiteral("尚未扫描")),
            presentation.summary, presentation.reviewRequired);
    }
}

void MainWindow::startRootRefresh(const QStringList &roots)
{
    if (m_closing)
        return;
    if (!m_libraryDatabaseReady) {
        m_libraryPanel->setImportError(QStringLiteral("作品库尚未成功打开。"));
        return;
    }
    if (libraryScanBusy()) {
        m_libraryPanel->setImportError(QStringLiteral("已有扫描或导入任务，请等待完成或停止当前任务。"));
        return;
    }
    // Known works are verified even without discovery roots. Only registered
    // roots may discover/import additional works during this refresh.
    const auto managed = m_scanRootStore->roots();
    QStringList requested;
    for (const auto &root : roots) {
        const auto normalized = ScanRootStore::normalizePath(root);
        bool known = false;
        for (const auto &candidate : managed)
            known = known || ScanRootStore::pathKey(candidate) == ScanRootStore::pathKey(normalized);
        if (!known) {
            m_libraryPanel->setImportError(QStringLiteral("扫描根配置已变化，请重新选择。"));
            return;
        }
        requested.push_back(normalized);
    }
    m_libraryImportState = LibraryImportState::Scanning;
    m_libraryPanel->setImportFeedback({});
    const auto accepted = m_libraryRootRefreshService->start(requested);
    if (accepted.status != LibraryImportSubmissionStatus::Accepted) {
        m_libraryPanel->setImportError(QStringLiteral("当前无法开始作品库刷新。"));
        return;
    }
    for (const auto &root : requested) {
        const auto key = ScanRootStore::pathKey(root);
        m_scanRootStatuses.insert(key, QStringLiteral("等待或正在扫描"));
        m_scanRootPresentations.insert(key, {QStringLiteral("扫描中"), false});
    }
    updateScanRootManager();
}

void MainWindow::requestLibraryWorkRemoval(qint64 workId)
{
    if (m_closing || !m_libraryDatabaseReady || libraryScanBusy()
        || m_libraryQueryInProgress || m_libraryQueryPending || workId <= 0
        || m_removedLibraryWorkIds.contains(workId))
        return;
    if (m_workRemovalConfirmation) {
        m_workRemovalConfirmation->raise();
        m_workRemovalConfirmation->activateWindow();
        return;
    }
    const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &work) { return work.id == workId; });
    if (found == m_libraryWorksSnapshot.cend()) return;
    const LibraryWork expected = *found;
    PlayerMessageBox dialog(QMessageBox::Warning, QStringLiteral("删除作品"),
        QStringLiteral("从作品库移除《%1》？\n\n"
            "本地音频、字幕和图片不会删除。该作品的自定义标题、字幕绑定、首选、播放进度和手动封面设置将被清除。\n"
            "如果作品仍在扫描根目录内，后续扫描可能重新导入。").arg(expected.displayName),
        QMessageBox::NoButton, this);
    dialog.setObjectName(QStringLiteral("workRemovalConfirmation"));
    dialog.setTextFormat(Qt::PlainText);
    auto *remove = dialog.addButton(QStringLiteral("删除"), QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("confirmWorkRemovalButton"));
    auto *cancel = dialog.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    cancel->setObjectName(QStringLiteral("cancelWorkRemovalButton"));
    dialog.setDefaultButton(cancel);
    dialog.setEscapeButton(cancel);
    m_workRemovalConfirmation = &dialog;
    const auto resetConfirmation = qScopeGuard([this] { m_workRemovalConfirmation = nullptr; });
    dialog.exec();
    if (dialog.clickedButton() != remove || m_closing || !m_libraryDatabaseReady
        || libraryScanBusy() || m_libraryQueryInProgress || m_libraryQueryPending)
        return;
    const auto current = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &work) { return work.id == workId; });
    if (current == m_libraryWorksSnapshot.cend() || current->rootPath != expected.rootPath
        || current->importedAtMs != expected.importedAtMs || m_removedLibraryWorkIds.contains(workId)) {
        m_libraryPanel->setImportError(QStringLiteral("作品已变化，请重新选择后删除。"));
        return;
    }
    const auto submission = m_libraryImportService->submitManualRemoval(workId, expected.rootPath);
    if (submission.status != LibraryImportSubmissionStatus::Accepted) {
        m_libraryPanel->setImportError(QStringLiteral("当前无法删除作品，请等待已有任务结束后重试。"));
        return;
    }
    m_libraryTaskId = submission.taskId;
    m_libraryManualRemovalTaskId = submission.taskId;
    m_libraryManualRemovalWorkId = workId;
    m_libraryImportState = LibraryImportState::Scanning;
    m_libraryScanSummary.clear();
    m_libraryPanel->setWorkRemovalActive(true);
    m_libraryPanel->setImportFeedback({});
    m_libraryPanel->setImportState(m_libraryImportState, QStringLiteral("正在核对待删除作品…"), true);
    updateScanRootManager();
}

void MainWindow::forgetRemovedLibraryWork(qint64 workId)
{
    if (workId <= 0 || m_removedLibraryWorkIds.contains(workId))
        return;
    m_removedLibraryWorkIds.insert(workId);
    invalidateTrackDurations(workId);
    forgetCoverSelection(workId);
    m_playbackQueue.invalidateWork(workId);
    m_queueAppendDuringReplacement.removeIf([workId](const auto &entry) { return entry.workId == workId; });
    updatePlaybackQueue();
    if ((m_queuePreparationInProgress || m_queuePreparationPending)
        && m_queuePreparationRequest.generation == m_audioOpenGeneration
        && m_queuePreparationRequest.workId == workId) {
        ++m_audioOpenGeneration;
        m_queuePreparationPending = false;
    }
    // A failed old request may arrive before the import worker's queued commit
    // notification. Remove only errors belonging to the confirmed deletion.
    const auto removeErrors = [workId](auto &byWork, QString &message) {
        if (!byWork.remove(workId)) return;
        QStringList remaining;
        for (const auto &lines : byWork) remaining.append(lines);
        message = remaining.join(QLatin1Char('\n'));
    };
    removeErrors(m_progressCloseFailuresByWork, m_progressCloseFailure);
    removeErrors(m_subtitleCloseFailuresByWork, m_subtitlePreferenceCloseFailure);
    // Accepted writes can already be running. Their transactions reject deleted
    // IDs; discard queued edits and ignore obsolete replies without retargeting.
    m_preferenceRequests.removeIf([workId](const auto &request) { return request.workId == workId; });
    if ((m_audioOpenInProgress || m_audioOpenPending)
        && m_audioRequestedGeneration == m_audioOpenGeneration
        && m_audioRequestedWorkId == workId) {
        ++m_audioOpenGeneration;
        m_audioOpenPending = false;
        m_audioRequestedWorkId = 0;
        m_audioRequestedFileId = 0;
    }
    if (m_rootSubtitleWaitWorkId == workId)
        m_rootSubtitleWaitAudioGeneration = 0;
    if (m_playingLibraryWorkId != workId)
        return;

    cancelPlaybackRestore(false);
    m_progressSessionActive = false;
    m_progressLastWriteRequest = 0;
    m_progressHasQueuedSave = false;
    m_playingLibraryWorkId = 0;
    m_playingLibraryFileId = 0;
    m_libraryPanel->setPlayingTrack(0, 0);
    resetBoundSubtitleContext();
    m_subtitleQueryPending = false;
    // An external temporary subtitle belongs to the current media session.
    // Keep its pending load as well as any already parsed subtitle text.
    if (!m_manualSubtitleChoice) {
        ++m_subtitleRequestGeneration;
        m_subtitleLoadPending = false;
        m_subtitlePanel->setLoading(false);
    }
    m_librarySubtitleResolution = {};
    m_lastLibrarySubtitleLoad = {};
    m_subtitlePreferenceFeedback.clear();
    m_subtitlePreferenceFeedbackError = false;
    m_subtitlePanel->setSelectionEnabled(false);
    updateSubtitleActions();
    setLibrarySubtitleStatus(QStringLiteral("作品已从作品库移除；当前已加载字幕保持。"));
}

void MainWindow::applyRootRefreshFinished(const LibraryRootRefreshResult &result)
{
    if (result.taskId != m_libraryRootRefreshService->taskId() || m_closing)
        return;
    if (!result.cancelled) m_scanRootInspectionPending = true;
    QStringList details;
    qsizetype suppressed = 0;
    const auto detail = [&details, &suppressed](const QString &text) {
        if (details.size() < 200) details.append(text);
        else ++suppressed;
    };
    qsizetype added = 0, updated = 0, removed = 0, skipped = 0, failed = 0, overlaps = 0, associationFailed = 0;
    const auto &verification = result.workVerification;
    removed += verification.removed;
    failed += verification.failed + (verification.errorMessage.isEmpty() ? 0 : 1);
    if (!verification.errorMessage.isEmpty())
        detail(QStringLiteral("在库作品核验：%1").arg(verification.errorMessage));
    for (const auto &message : verification.diagnostics) detail(message);
    for (const auto &item : verification.items) {
        if (!item.errorMessage.isEmpty())
            detail(item.directory + QStringLiteral("：") + item.errorMessage);
        for (const auto &message : item.diagnostics) detail(message);
    }
    if (verification.notStarted > 0)
        detail(QStringLiteral("在库作品核验尚有 %1 项未开始，记录保留。").arg(verification.notStarted));
    for (const auto &root : result.roots) {
        added += root.imported;
        updated += root.updated;
        removed += root.removed;
        skipped += root.skipped;
        failed += root.failed + (root.errorMessage.isEmpty() ? 0 : 1);
        overlaps += root.overlapDirectories.size();
        associationFailed += root.associationFailed;
        QString status = QStringLiteral("新增 %1，更新 %2，已清理作品 %3，跳过 %4，失败 %5，重叠待确认 %6，字幕关联异常 %7")
            .arg(root.imported).arg(root.updated).arg(root.removed).arg(root.skipped)
            .arg(root.failed).arg(root.overlapDirectories.size()).arg(root.associationFailed);
        if (root.cancelled)
            status.prepend(QStringLiteral("已停止，尚未完成的检查未应用。\n"));
        if (!root.errorMessage.isEmpty())
            status += QStringLiteral("\n本轮未完成：%1").arg(root.errorMessage);
        if (root.notStarted > 0)
            status += QStringLiteral("\n未开始 %1 项。").arg(root.notStarted);
        const bool interruptedByError = !root.success && !result.errorMessage.isEmpty();
        if (interruptedByError)
            status += QStringLiteral("\n扫描未完成，后续项目未开始：%1").arg(result.errorMessage);
        const bool incomplete = root.notStarted > 0;
        const bool hasIssue = !root.errorMessage.isEmpty() || root.failed || root.associationFailed
            || interruptedByError;
        const bool reviewRequired = !root.overlapDirectories.isEmpty();
        QString shortStatus = root.cancelled ? QStringLiteral("已停止")
            : hasIssue ? QStringLiteral("有异常")
            : incomplete ? QStringLiteral("未完成")
            : reviewRequired ? QStringLiteral("待确认") : QStringLiteral("完成");
        if (hasIssue && root.cancelled)
            shortStatus += QStringLiteral(" · 有异常");
        if (reviewRequired && shortStatus != QStringLiteral("待确认"))
            shortStatus += QStringLiteral(" · 待确认");
        const auto key = ScanRootStore::pathKey(root.rootDirectory);
        m_scanRootStatuses.insert(key, status);
        m_scanRootPresentations.insert(key, {shortStatus, reviewRequired});
        detail(root.rootDirectory + QStringLiteral("：") + status);
        for (const auto &message : root.diagnostics) detail(message);
        for (const auto &item : root.items) {
            if (!item.errorMessage.isEmpty())
                detail(item.directory + QStringLiteral("：") + item.errorMessage);
            for (const auto &message : item.diagnostics) detail(message);
        }
    }
    if (!result.errorMessage.isEmpty())
        details.prepend(result.errorMessage);
    const auto summary = QStringLiteral("%1 新增 %2，更新 %3，已清理作品 %4，跳过 %5，失败 %6，重叠待确认 %7，字幕关联异常 %8（已提交的作品更新保留）。")
        .arg(result.cancelled ? QStringLiteral("刷新已停止。") : QStringLiteral("刷新完成。"))
        .arg(added).arg(updated).arg(removed).arg(skipped).arg(failed).arg(overlaps).arg(associationFailed);
    if (suppressed)
        details.append(QStringLiteral("另有 %1 条诊断未显示。").arg(suppressed));
    m_libraryPanel->setImportFeedback({}, details.join(QLatin1Char('\n')));
    m_libraryImportState = result.cancelled ? LibraryImportState::Cancelled
        : failed || associationFailed || !result.errorMessage.isEmpty()
            ? LibraryImportState::Failed : LibraryImportState::Completed;
    m_libraryPanel->setImportState(m_libraryImportState, summary, false);
    if (result.cancelled && (failed || associationFailed || !result.errorMessage.isEmpty()))
        m_libraryPanel->setImportError(summary + (result.errorMessage.isEmpty()
            ? QString() : QLatin1Char('\n') + result.errorMessage));
    refreshLibrary(); // Preserve browsing and playback identities.
    updateScanRootManager();
    QTimer::singleShot(0, this, &MainWindow::tryStartScanRootInspection);
}

void MainWindow::scheduleStartupRootRefresh()
{
    if (m_startupRootScanScheduled || m_closing || !m_scanRootSettingsReady || !m_libraryDatabaseReady)
        return;
    m_startupRootScanScheduled = true;
    m_startupRootScanPending = m_scanRootStore->startupEnabled() && !m_scanRootStore->roots().isEmpty();
    if (m_startupRootScanPending)
        QTimer::singleShot(0, this, &MainWindow::tryStartStartupRootRefresh);
}

void MainWindow::tryStartStartupRootRefresh()
{
    if (!m_startupRootScanPending || m_closing || !m_libraryDatabaseReady || libraryScanBusy())
        return;
    m_startupRootScanPending = false;
    if (m_scanRootStore->startupEnabled())
        startRootRefresh(m_scanRootStore->roots());
}
} // namespace asmr
