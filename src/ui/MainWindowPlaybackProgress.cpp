#include "ui/MainWindow.h"
#include "ui/PlayerMessageBox.h"

#include "playback/AudioPlayer.h"
#include "playback/PlaybackProgressCoordinator.h"
#include "ui/PlaybackControlsWidget.h"

#include <QDebug>
#include <QTimer>

#include <algorithm>

namespace asmr {
namespace {
constexpr qint64 SaveIntervalMs = 10'000;
constexpr qint64 MeaningfulChangeMs = 1'000;
constexpr qint64 SeekToleranceMs = 250;
constexpr int RestoreTimeoutMs = 10'000;
QString progressDiagnostics(const LibraryOperationResult &result)
{
    QStringList lines;
    for (const auto &diagnostic : result.diagnostics)
        lines.append(diagnostic.message);
    return lines.join(QLatin1Char('\n'));
}
qint64 distance(qint64 left, qint64 right)
{
    return left >= right ? left - right : right - left; // Nonnegative media values.
}
bool mediaLoaded(QMediaPlayer::MediaStatus status)
{
    return status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia
        || status == QMediaPlayer::BufferingMedia || status == QMediaPlayer::StalledMedia;
}
}

void MainWindow::connectPlaybackProgress()
{
    m_playbackProgress = new PlaybackProgressCoordinator(m_libraryDatabasePath, this);
    connect(m_playbackProgress, &PlaybackProgressCoordinator::completed,
            this, &MainWindow::applyPlaybackProgressResult);
    connect(m_playbackProgress, &PlaybackProgressCoordinator::shutdownFinished, this, [this] {
        m_progressShutdownFinished = true;
        finishCloseIfReady();
    });
}

void MainWindow::beginPlaybackProgress(bool fromBeginning)
{
    ++m_progressRequestGeneration;
    m_progressQueryRequest = 0;
    m_progressLastWriteRequest = 0;
    m_progressSessionActive = m_playingLibraryWorkId > 0 && m_playingLibraryFileId > 0;
    m_progressSource = m_audioPlayer->source();
    m_savedPlaybackProgress.reset();
    m_progressQueryPending = false;
    m_progressRestorePending = false;
    m_progressSeekIssued = false;
    m_progressClearRequested = false;
    m_progressError = false;
    m_progressHasQueuedSave = false;
    m_progressLastQueuedPosition = 0;
    m_progressLastQueuedDuration.reset();
    m_progressClock.start();
    m_progressLastAttemptMs = 0;
    m_playbackControls->setResumePending(false);
    if (!m_progressSessionActive || fromBeginning) {
        if (fromBeginning) m_audioPlayer->setPosition(0);
        m_audioPlayer->play();
        return;
    }
    m_progressQueryPending = true;
    m_playbackControls->setResumePending(true);
    m_progressQueryRequest = m_playbackProgress->query(m_playingLibraryWorkId, m_playingLibraryFileId,
                                                     m_audioGeneration);
    const auto audio = m_audioGeneration;
    const auto request = m_progressRequestGeneration;
    QTimer::singleShot(RestoreTimeoutMs, this, [this, audio, request] {
        if (!m_closing && audio == m_audioGeneration && request == m_progressRequestGeneration
            && m_progressQueryPending) {
            cancelPlaybackRestore(false);
            startPlaybackFromProgressDecision(QStringLiteral("进度查询超时，已从头播放；未覆盖原记录。"));
        }
    });
    if (!m_progressQueryRequest) {
        cancelPlaybackRestore(false);
        startPlaybackFromProgressDecision(QStringLiteral("无法查询播放进度，已从头播放。"));
    }
}

void MainWindow::applyPlaybackProgressResult(const PlaybackProgressTaskResult &result)
{
    if (m_removedLibraryWorkIds.contains(result.workId)) {
        if (m_closing)
            finishCloseIfReady();
        return;
    }
    if (result.kind != PlaybackProgressTaskKind::Query && !result.success) {
        const auto message = QStringLiteral("播放进度%1失败：%2")
            .arg(result.kind == PlaybackProgressTaskKind::Clear ? QStringLiteral("清除") : QStringLiteral("保存"),
                 progressDiagnostics(result));
        qWarning().noquote() << message;
        if (m_closing) {
            m_progressCloseFailuresByWork[result.workId].append(message);
            m_progressCloseFailure += (m_progressCloseFailure.isEmpty() ? QString() : QStringLiteral("\n")) + message;
        }
    }
    const bool current = !m_closing && !m_progressSourceChanging && m_progressSessionActive
        && result.session == m_audioGeneration && result.workId == m_playingLibraryWorkId
        && result.audioFileId == m_playingLibraryFileId && m_progressSource == m_audioPlayer->source();
    if (current && result.kind == PlaybackProgressTaskKind::Query
        && result.requestId == m_progressQueryRequest && m_progressQueryPending) {
        m_progressQueryPending = false;
        if (!result.success) {
            startPlaybackFromProgressDecision(
                QStringLiteral("读取进度失败：%1；已从头播放。").arg(progressDiagnostics(result)));
        } else if (!result.progress || result.progress->positionMs <= 0) {
            startPlaybackFromProgressDecision({});
        } else {
            m_savedPlaybackProgress = result.progress;
            beginAutomaticPlaybackRestore();
        }
    } else if (current && result.kind != PlaybackProgressTaskKind::Query
               && result.requestId == m_progressLastWriteRequest && !result.success)
        m_progressHasQueuedSave = false; // A later NEW observation may be saved; never retry this payload.
    if (m_closing)
        finishCloseIfReady();
}

void MainWindow::startPlaybackFromProgressDecision(const QString &diagnostic)
{
    m_progressQueryPending = false;
    m_progressRestorePending = false;
    m_progressSeekIssued = false;
    m_savedPlaybackProgress.reset();
    m_playbackControls->setResumePending(false);
    m_progressLastAttemptMs = m_progressClock.isValid() ? m_progressClock.elapsed() : 0;
    if (!diagnostic.isEmpty())
        qWarning().noquote() << diagnostic;
    if (!m_closing && !m_progressError && m_audioPlayer->error() == QMediaPlayer::NoError)
        m_audioPlayer->play();
}

void MainWindow::cancelPlaybackRestore(bool startPlayback)
{
    ++m_progressRequestGeneration;
    const bool waiting = m_progressQueryPending || m_progressRestorePending;
    m_progressQueryRequest = 0;
    m_progressQueryPending = false;
    m_progressRestorePending = false;
    m_progressSeekIssued = false;
    m_savedPlaybackProgress.reset();
    m_playbackControls->setResumePending(false);
    if (startPlayback && waiting)
        startPlaybackFromProgressDecision({});
}

void MainWindow::beginAutomaticPlaybackRestore()
{
    if (m_closing || !m_progressSessionActive || !m_savedPlaybackProgress)
        return;
    ++m_progressRequestGeneration;
    m_progressRestorePending = true;
    m_progressSeekIssued = false;
    m_playbackControls->setResumePending(true);
    const auto audio = m_audioGeneration;
    const auto request = m_progressRequestGeneration;
    QTimer::singleShot(RestoreTimeoutMs, this, [this, audio, request] {
        if (!m_closing && audio == m_audioGeneration && request == m_progressRequestGeneration
            && m_progressRestorePending) {
            cancelPlaybackRestore(false);
            m_audioPlayer->setPosition(0);
            startPlaybackFromProgressDecision(QStringLiteral("恢复定位未完成，已从头播放；原记录保留。"));
        }
    });
    tryRestorePlaybackProgress();
}

void MainWindow::tryRestorePlaybackProgress()
{
    if (m_closing || m_progressSourceChanging || !m_progressSessionActive || m_progressError
        || m_progressSource != m_audioPlayer->source() || !m_savedPlaybackProgress
        || !m_progressRestorePending)
        return;
    if (m_audioPlayer->error() != QMediaPlayer::NoError)
        return;
    const qint64 duration = m_audioPlayer->duration();
    if (!mediaLoaded(m_audioPlayer->mediaStatus()) || duration <= 0)
        return;
    const auto &saved = *m_savedPlaybackProgress;
    const bool changed = saved.durationMs && *saved.durationMs > 0
        && distance(*saved.durationMs, duration) > std::max(qint64(1000), duration / 100);
    if (saved.positionMs <= 0 || saved.positionMs >= duration || changed) {
        cancelPlaybackRestore(false);
        m_audioPlayer->setPosition(0);
        startPlaybackFromProgressDecision(changed ? QStringLiteral("媒体时长与保存时不同，已从头播放。")
                                                 : QStringLiteral("保存位置超出当前媒体范围，已从头播放。"));
        return;
    }
    if (!m_audioPlayer->isSeekable())
        return;
    if (!m_progressSeekIssued) {
        m_progressRestoreTarget = saved.positionMs;
        m_progressSeekIssued = true;
        m_audioPlayer->setPosition(m_progressRestoreTarget);
        if (!m_progressRestorePending) return; // A synchronous position notification may finish it.
    }
    const qint64 position = m_audioPlayer->position();
    if (position > 0 && distance(position, m_progressRestoreTarget) <= SeekToleranceMs) {
        m_progressLastQueuedPosition = position;
        m_progressHasQueuedSave = true; // The restored observation is already durable.
        m_progressLastQueuedDuration = duration;
        startPlaybackFromProgressDecision({});
        refreshSubtitles();
    }
}

void MainWindow::onProgressPosition(qint64 position)
{
    if (!m_closing && !m_progressSourceChanging && position > 0
        && m_audioPlayer->playbackState() == QMediaPlayer::PlayingState
        && m_audioPlayer->mediaStatus() != QMediaPlayer::EndOfMedia
        && m_audioPlayer->error() == QMediaPlayer::NoError)
        m_completedAudioIdentities.remove(playbackProgressIdentity(m_playingLibraryWorkId,
            m_playingLibraryFileId, m_audioPlayer->source().toLocalFile()));
    if (m_closing || m_progressSourceChanging || !m_progressSessionActive || m_progressError
        || m_progressSource != m_audioPlayer->source() || position < 0)
        return;
    if (m_progressRestorePending) {
        tryRestorePlaybackProgress();
        return;
    }
    if (m_progressQueryPending)
        return;
    if (position > 0 && m_audioPlayer->playbackState() != QMediaPlayer::StoppedState)
        m_progressClearRequested = false;
    if (m_audioPlayer->playbackState() == QMediaPlayer::PlayingState)
        saveCurrentPlaybackProgress(false);
}

void MainWindow::saveCurrentPlaybackProgress(bool force)
{
    if (!m_playbackProgress || m_closing || !m_progressSessionActive || m_progressSourceChanging
        || m_progressQueryPending || m_progressRestorePending
        || m_progressClearRequested || m_progressError || m_audioPlayer->error() != QMediaPlayer::NoError
        || m_progressSource != m_audioPlayer->source())
        return;
    const qint64 position = m_audioPlayer->position();
    if (position <= 0 || m_audioPlayer->mediaStatus() == QMediaPlayer::EndOfMedia)
        return;
    const qint64 duration = m_audioPlayer->duration();
    const auto knownDuration = duration > 0 ? std::optional<qint64>(duration) : std::nullopt;
    const auto elapsed = m_progressClock.elapsed();
    if (!force && (elapsed - m_progressLastAttemptMs < SaveIntervalMs
        || distance(position, m_progressLastQueuedPosition) < MeaningfulChangeMs))
        return;
    if (m_progressHasQueuedSave && position == m_progressLastQueuedPosition && knownDuration == m_progressLastQueuedDuration)
        return;
    m_progressLastWriteRequest = m_playbackProgress->save(m_playingLibraryWorkId, m_playingLibraryFileId,
                                                        m_audioGeneration, position, knownDuration);
    if (m_progressLastWriteRequest) {
        m_progressLastQueuedPosition = position;
        m_progressLastQueuedDuration = knownDuration;
        m_progressHasQueuedSave = true;
        m_progressLastAttemptMs = elapsed;
    }
}

void MainWindow::clearCurrentPlaybackProgress(bool explicitRequest)
{
    if (!m_playbackProgress || m_closing || !m_progressSessionActive || m_progressSourceChanging
        || (m_progressClearRequested && !explicitRequest))
        return;
    cancelPlaybackRestore(false);
    m_progressClearRequested = true;
    m_progressHasQueuedSave = false;
    m_progressLastQueuedPosition = 0;
    m_progressLastAttemptMs = m_progressClock.elapsed();
    m_progressLastWriteRequest = m_playbackProgress->clear(m_playingLibraryWorkId, m_playingLibraryFileId,
                                                         m_audioGeneration);
}

void MainWindow::onProgressState(QMediaPlayer::PlaybackState state)
{
    if (m_closing || m_progressSourceChanging || !m_progressSessionActive
        || m_progressSource != m_audioPlayer->source())
        return;
    // A stopped seek can report its position before playback starts. A real
    // new PlayingState is also a fresh observation, even when Pause follows
    // before the backend emits another position notification.
    if (state == QMediaPlayer::PlayingState && state == m_audioPlayer->playbackState()
        && m_audioPlayer->position() > 0 && !m_progressQueryPending
        && !m_progressRestorePending && !m_progressError
        && m_audioPlayer->error() == QMediaPlayer::NoError
        && m_audioPlayer->mediaStatus() != QMediaPlayer::EndOfMedia)
        m_progressClearRequested = false;
    if (state == QMediaPlayer::PausedState)
        saveCurrentPlaybackProgress(true);
}

void MainWindow::onProgressMediaStatus(QMediaPlayer::MediaStatus status)
{
    if (m_closing || m_progressSourceChanging || !m_progressSessionActive)
        return;
    if (status == QMediaPlayer::InvalidMedia || m_audioPlayer->error() != QMediaPlayer::NoError) {
        m_progressError = true;
        cancelPlaybackRestore(false);
    } else if (status == QMediaPlayer::EndOfMedia && m_audioPlayer->mediaStatus() == status) {
        clearCurrentPlaybackProgress();
    } else
        tryRestorePlaybackProgress();
}

void MainWindow::finishPlaybackProgressForClose()
{
    if (m_progressClosingFinalized)
        return;
    if (m_progressSessionActive && !m_progressError && m_audioPlayer->error() == QMediaPlayer::NoError) {
        if (m_audioPlayer->mediaStatus() == QMediaPlayer::EndOfMedia)
            clearCurrentPlaybackProgress();
        else
            saveCurrentPlaybackProgress(true);
    }
    m_progressClosingFinalized = true;
    cancelPlaybackRestore(false);
    if (m_playbackProgress)
        m_playbackProgress->shutdown();
}

bool MainWindow::acknowledgePlaybackProgressCloseFailure()
{
    if ((m_progressCloseFailure.isEmpty() && m_subtitlePreferenceCloseFailure.isEmpty())
        || m_progressCloseFailureAcknowledged)
        return true;
    if (!m_progressCloseErrorDialog) {
        const auto failures = m_progressCloseFailure + (m_progressCloseFailure.isEmpty()
            || m_subtitlePreferenceCloseFailure.isEmpty() ? QString() : QStringLiteral("\n"))
            + m_subtitlePreferenceCloseFailure;
        // Reuse the existing close acknowledgement after every persistence queue drains.
        m_progressCloseErrorDialog = new PlayerMessageBox(QMessageBox::Warning, QStringLiteral("数据保存出现错误"),
            QStringLiteral("部分播放进度或字幕设置保存失败，未能持久化。关闭后可能无法保留预期设置。\n\n%1")
                .arg(failures), QMessageBox::Close, this);
        m_progressCloseErrorDialog->setObjectName(QStringLiteral("playbackProgressCloseErrorDialog"));
        m_progressCloseErrorDialog->setTextFormat(Qt::PlainText);
        connect(m_progressCloseErrorDialog, &QMessageBox::finished, this, [this] {
            m_progressCloseFailureAcknowledged = true;
            finishCloseIfReady();
        });
        m_progressCloseErrorDialog->open(); // Never a nested GUI event loop.
    }
    return false;
}
} // namespace asmr
