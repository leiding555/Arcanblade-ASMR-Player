#include "ui/MainWindow.h"
#include "ui/ChickenRiceTranslateDialog.h"
#include "ui/LibraryPanel.h"
#include "ai/ChickenRiceTranslateService.h"
#include <QTimer>
#include "playback/AudioPlayer.h"
#include <QAction>
#include <QDir>
#include <QMenu>
#include <QSet>
#include <algorithm>

namespace asmr {
namespace {
bool sameAudioPath(const QString &first, const QString &second)
{
    if (first.isEmpty() || second.isEmpty()) return false;
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir::fromNativeSeparators(first)).compare(
        QDir::cleanPath(QDir::fromNativeSeparators(second)), sensitivity) == 0;
}
}

void MainWindow::connectChickenRiceTranslation()
{
    auto *action = new QAction(QStringLiteral("AI 字幕…"), m_libraryPanel);
    action->setObjectName(QStringLiteral("aiSubtitleAction"));
    connect(action, &QAction::triggered, this, [this] { showChickenRiceTranslation(); });
    connect(m_libraryPanel, &LibraryPanel::aiSubtitlesRequested, action, &QAction::trigger);
    connect(m_libraryPanel, &LibraryPanel::translateTracksRequested,
            this, &MainWindow::translateLibraryTracks);
    connect(m_libraryPanel, &LibraryPanel::generateSubtitlesRequested,
            this, &MainWindow::generateLibrarySubtitles);
}

void MainWindow::showChickenRiceTranslation(const QStringList &audioPaths)
{
    if (m_closing) return;
    if (!m_chickenRiceTranslateDialog) {
        m_chickenRiceTranslateDialog = new ChickenRiceTranslateDialog(
            &m_settings, this, nullptr, nullptr, m_gpuDetectionProbe, {}, nullptr, nullptr, nullptr, m_libraryDatabasePath);
        connect(m_chickenRiceTranslateDialog, &ChickenRiceTranslateDialog::shutdownFinished,
                this, &MainWindow::finishCloseIfReady);
        connect(m_chickenRiceTranslateDialog, &ChickenRiceTranslateDialog::batchFinished,
                this, &MainWindow::queueAiSubtitleRefresh);
    }
    m_chickenRiceTranslateDialog->addAudioFiles(audioPaths);
    m_chickenRiceTranslateDialog->setCurrentAudioPath(
        !m_audioOpenInProgress && !m_audioOpenPending && m_audioGeneration > 0
            ? m_audioPlayer->source().toLocalFile() : QString());
    m_chickenRiceTranslateDialog->show();
    m_chickenRiceTranslateDialog->raise();
    m_chickenRiceTranslateDialog->activateWindow();
}

void MainWindow::translateLibraryTracks(qint64 workId, const QVector<qint64> &fileIds)
{
    generateLibrarySubtitles(workId, fileIds, {});
}

void MainWindow::generateLibrarySubtitles(qint64 workId, const QVector<qint64> &fileIds,
                                         const QString &modeId)
{
    if (!modeId.isEmpty() && modeId != QStringLiteral("offline-direct")
        && modeId != QStringLiteral("offline-asr-ja")) return;
    if (m_closing || fileIds.isEmpty() || !m_libraryDatabaseReady
        || m_libraryQueryInProgress || m_libraryQueryPending
        || workId <= 0 || workId != m_librarySnapshotWorkId
        || workId != m_libraryPanel->selectedWorkId()) return;
    const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &entry) { return entry.id == workId; });
    if (work == m_libraryWorksSnapshot.cend() || work->rootPath.isEmpty()
        || m_removedLibraryWorkIds.contains(workId)) return;
    QStringList relativePaths;
    QSet<qint64> seen;
    for (const auto id : fileIds) {
        if (seen.contains(id)) continue;
        const auto file = std::find_if(m_libraryFilesSnapshot.cbegin(), m_libraryFilesSnapshot.cend(),
            [id](const auto &entry) { return entry.id == id; });
        if (id <= 0 || file == m_libraryFilesSnapshot.cend() || file->workId != workId
            || file->type != LibraryFileType::Audio || file->missing) return;
        const auto relative = QDir::fromNativeSeparators(file->relativePath);
        const auto parts = relative.split(QLatin1Char('/'));
        if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(QLatin1Char(':'))
            || relative.contains(QChar::Null) || parts.contains(QStringLiteral(".."))
            || parts.contains(QStringLiteral(".")) || parts.contains(QString())) return;
        seen.insert(id);
        relativePaths.append(relative);
    }
    showChickenRiceTranslation();
    if (!m_closing && m_chickenRiceTranslateDialog) {
        if (modeId.isEmpty()) m_chickenRiceTranslateDialog->addLibraryAudioFiles(work->rootPath, relativePaths);
        else m_chickenRiceTranslateDialog->addLibraryAudioFilesForMode(work->rootPath, relativePaths, modeId);
    }
}

void MainWindow::queueAiSubtitleRefresh(const QVector<AiSubtitleTaskResult> &results)
{
    if (m_closing)
        return;
    for (const auto &result : results) {
        if (result.state != AiSubtitleTaskState::Succeeded
            || !QDir::isAbsolutePath(result.audioPath) || !QDir::isAbsolutePath(result.srtPath))
            continue;
        m_aiSubtitlePendingResults.insert(QDir::cleanPath(QDir::fromNativeSeparators(result.audioPath)),
            QDir::cleanPath(QDir::fromNativeSeparators(result.srtPath)));
    }
    tryStartAiSubtitleRefresh();
}

void MainWindow::tryStartAiSubtitleRefresh()
{
    if (m_closing || !m_libraryDatabaseReady || libraryScanBusy()
        || m_libraryQueryInProgress || m_libraryQueryPending)
        return;
    // Resolve only against a settled catalog. A batch can finish while a newly
    // imported work's SQL query is pending; retain paths until that reply arrives.
    // Individually imported works require no managed scan root.
    for (auto result = m_aiSubtitlePendingResults.cbegin(); result != m_aiSubtitlePendingResults.cend(); ++result) {
        for (const auto &work : m_libraryWorksSnapshot) {
            if (work.id <= 0 || m_removedLibraryWorkIds.contains(work.id))
                continue;
            auto prefix = QDir::cleanPath(QDir::fromNativeSeparators(work.rootPath));
            if (!prefix.endsWith(QLatin1Char('/')))
                prefix += QLatin1Char('/');
#ifdef Q_OS_WIN
            constexpr auto sensitivity = Qt::CaseInsensitive;
#else
            constexpr auto sensitivity = Qt::CaseSensitive;
#endif
            if (result.key().startsWith(prefix, sensitivity) && result.value().startsWith(prefix, sensitivity))
                m_aiSubtitlePendingWorks.insert(work.id, work.rootPath);
        }
    }
    m_aiSubtitlePendingResults.clear();
    while (!m_aiSubtitlePendingWorks.isEmpty()) {
        const auto entry = m_aiSubtitlePendingWorks.cbegin();
        const auto id = entry.key();
        const auto root = entry.value();
        if (m_removedLibraryWorkIds.contains(id)) {
            m_aiSubtitlePendingWorks.remove(id);
            continue;
        }
        const auto submission = m_libraryImportService->submitRefresh(id, root);
        if (submission.status == LibraryImportSubmissionStatus::Busy)
            return; // All coordinators wake this queue after their final event.
        m_aiSubtitlePendingWorks.remove(id);
        if (submission.status != LibraryImportSubmissionStatus::Accepted) {
            m_libraryPanel->setImportError(QStringLiteral("字幕已保存，但暂时无法更新作品库；可稍后重新扫描作品。"));
            continue;
        }
        m_libraryTaskId = submission.taskId;
        m_aiSubtitleRefreshTaskId = submission.taskId;
        m_libraryImportState = LibraryImportState::Scanning;
        m_libraryScanSummary.clear();
        m_libraryPanel->setImportFeedback({});
        m_libraryPanel->setImportState(m_libraryImportState,
            QStringLiteral("正在更新生成字幕所属的作品…"), true);
        updateScanRootManager();
        return;
    }
}

void MainWindow::loadChickenRiceResult(const QString &audioPath, const QString &srtPath)
{
    // Recheck at the point of explicit use, including a newer pending play intention.
    // requestManualSubtitle retains existing async read/generation and temporary-load semantics.
    if (m_closing || !m_audioGeneration || m_audioOpenInProgress || m_audioOpenPending
        || !sameAudioPath(audioPath, m_audioPlayer->source().toLocalFile())
        || srtPath.isEmpty()) return;
    requestManualSubtitle(srtPath);
}
} // namespace asmr
