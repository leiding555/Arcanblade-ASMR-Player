#include "ui/MainWindow.h"

#include "playback/AudioPlayer.h"
#include "ui/LibraryPanel.h"
#include "ui/PlaybackControlsWidget.h"
#include "ui/PlaybackQueuePanel.h"
#include "ui/PlaybackDetailWidget.h"
#include "ui/SubtitlePanel.h"

#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPromise>
#include <QSplitter>
#include <QSet>
#include <QSlider>
#include <QTextEdit>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <exception>

namespace asmr {
namespace {
struct QueuePreparationResult {
    bool success = false;
    bool unavailable = false;
    QString error;
    PlaybackQueueEntry entry;
    quint64 entryId = 0;
    std::optional<PlaybackQueue> replacement;
};

QString diagnostics(const LibraryOperationResult &result)
{
    QStringList lines;
    for (const auto &item : result.diagnostics) lines.append(item.message);
    return lines.join(QLatin1Char('\n'));
}

bool isPlaybackPanelBody(const QWidget *widget, const SubtitlePanel *subtitles,
                         const PlaybackQueuePanel *queue)
{
    const auto *text = subtitles->findChild<QTextEdit *>(QStringLiteral("subtitleTextDisplay"));
    const auto *list = queue->findChild<QListWidget *>(QStringLiteral("playbackQueueList"));
    // These two surfaces keep their own focus for transcript clicks and queue
    // navigation. Scrollbars and other editors retain their normal keys.
    return (text && text->isReadOnly() && (widget == text || widget == text->viewport()))
        || (list && (widget == list || widget == list->viewport()));
}
}

void MainWindow::connectPlaybackQueue()
{
    connect(m_libraryPanel, &LibraryPanel::directoryPlayRequested, this, &MainWindow::playDirectory);
    connect(m_libraryPanel, &LibraryPanel::trackEnqueueRequested, this, &MainWindow::enqueueLibraryTrack);
    connect(m_libraryPanel, &LibraryPanel::tracksEnqueueRequested, this, &MainWindow::enqueueLibraryTracks);
    connect(m_playbackControls, &PlaybackControlsWidget::previousRequested, this, [this] {
        playQueueEntry(m_playbackQueue.previousId(), PlaybackQueueNavigation::Previous);
    });
    connect(m_playbackControls, &PlaybackControlsWidget::nextRequested, this, [this] {
        playQueueEntry(m_playbackQueue.nextId(), PlaybackQueueNavigation::Next);
    });
    connect(m_playbackControls, &PlaybackControlsWidget::playbackModeRequested,
            this, &MainWindow::setPlaybackMode);
    connect(m_playbackControls, &PlaybackControlsWidget::toggleQueueRequested, this, [this] {
        showPlaybackQueue(m_queuePanel->isHidden());
    });
    connect(m_queuePanel, &PlaybackQueuePanel::closeRequested, this, [this] { showPlaybackQueue(false); });
    connect(m_queuePanel, &PlaybackQueuePanel::playRequested, this,
            [this](quint64 id) { playQueueEntry(id); });
    connect(m_queuePanel, &PlaybackQueuePanel::removeRequested, this, &MainWindow::removeQueueEntry);
    connect(m_queuePanel, &PlaybackQueuePanel::clearOthersRequested, this, &MainWindow::clearOtherQueueEntries);
    updatePlaybackQueue();
}

void MainWindow::showPlaybackQueue(bool visible)
{
    if (!m_queuePanel) return;
    if (visible) finishPlaybackDetailTransition();
    if (visible && !m_closing) {
        if (m_queuePanel->isHidden()) m_queueReturnFocus = QApplication::focusWidget();
        updateQueuePanelGeometry();
        m_queuePanel->show();
        m_queuePanel->raise();
        m_queuePanel->setFocus(Qt::OtherFocusReason);
    } else {
        m_queuePanel->hide();
        if (!m_closing && m_queueReturnFocus && m_queueReturnFocus->isVisible()
            && m_queueReturnFocus->isEnabled()) m_queueReturnFocus->setFocus(Qt::OtherFocusReason);
        m_queueReturnFocus.clear();
    }
    m_playbackControls->setQueueState(static_cast<int>(m_playbackQueue.entries().size()),
                                     !m_queuePanel->isHidden());
}

void MainWindow::updateQueuePanelGeometry()
{
    if (!m_queuePanel || !m_workspaceSplitter) return;
    // The overlay occupies only the content area: sidebar/footer and the
    // independently selected subtitle visibility are left in their layouts.
    const QWidget *anchor = m_playbackDetailVisible
        ? static_cast<QWidget *>(m_subtitlePanel) : static_cast<QWidget *>(m_workspaceSplitter);
    const auto origin = anchor->mapTo(this, QPoint());
    const QRect area(origin, anchor->size());
    const int panelWidth = qMin(400, area.width());
    m_queuePanel->setGeometry(area.right() - panelWidth + 1, area.top(),
                              panelWidth, area.height());
    if (!m_queuePanel->isHidden()) m_queuePanel->raise();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (handlePlaybackDetailTransitionEvent(watched, event)) return true;
    if (m_playbackDetailVisible && (event->type() == QEvent::KeyPress
        || event->type() == QEvent::KeyRelease || event->type() == QEvent::ShortcutOverride)) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const auto *target = qobject_cast<QWidget *>(watched);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier
            && target && target->window() == this
            && (!QApplication::activeWindow() || QApplication::activeWindow() == this)
            && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()) {
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
                if (!m_queuePanel->isHidden()) showPlaybackQueue(false);
                else setPlaybackDetailVisible(false);
            }
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const auto *widget = qobject_cast<QWidget *>(watched);
        if (!m_closing && widget && widget->window() == this && m_queuePanel && m_playbackControls
            && m_queuePanel->isVisible()
            && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()) {
            const auto point = mouse->globalPosition().toPoint();
            const QRect queueBounds(m_queuePanel->mapToGlobal(QPoint()), m_queuePanel->size());
            const auto *toggle = m_playbackControls->findChild<QAbstractButton *>(
                QStringLiteral("togglePlaybackQueueButton"));
            const bool onToggle = toggle && toggle->isVisibleTo(this)
                && QRect(toggle->mapToGlobal(QPoint()), toggle->size()).contains(point);
            if (!queueBounds.contains(point) && !onToggle)
                showPlaybackQueue(false);
            // Keep the original click: dismissing the overlay does not own
            // the page's button, track selection or playback control event.
        }
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier
            && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()
            && isPlaybackPassiveClickTarget(widget, mouse->position().toPoint())) {
            // Passive areas release the old button focus. Reading/list panels
            // retain their own keyboard context for transcript clicks and queue navigation.
            if (widget == m_queuePanel || m_queuePanel->isAncestorOf(widget)) {
                m_queuePanel->setFocus(Qt::MouseFocusReason);
            } else if (widget == m_subtitlePanel || m_subtitlePanel->isAncestorOf(widget)) {
                if (auto *text = m_subtitlePanel->findChild<QTextEdit *>(
                        QStringLiteral("subtitleTextDisplay")))
                    text->setFocus(Qt::MouseFocusReason);
            } else {
                setFocus(Qt::MouseFocusReason);
            }
        }
    }
    if (handlePlaybackKeyEvent(watched, event)) return true;
    if ((watched == m_workspaceSplitter || watched == m_subtitlePanel
         || watched == m_playbackDetailPage)
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move
            || event->type() == QEvent::LayoutRequest))
        QTimer::singleShot(0, this, &MainWindow::updateQueuePanelGeometry);
    return QWidget::eventFilter(watched, event);
}

bool MainWindow::isPlaybackPassiveClickTarget(const QWidget *widget, const QPoint &position) const
{
    if (m_closing || !widget || widget->window() != this || !widget->isEnabled()) return false;
    const auto *sidebar = m_libraryPanel->sidebarWidget();
    const bool inSidebar = widget == sidebar || sidebar->isAncestorOf(widget);
    const bool inFooter = widget == m_playbackControls || m_playbackControls->isAncestorOf(widget);
    const auto *information = m_libraryPanel->findChild<QWidget *>(QStringLiteral("libraryWorkInformation"));
    const bool inInformation = information && (widget == information || information->isAncestorOf(widget));
    const bool inSubtitles = widget == m_subtitlePanel || m_subtitlePanel->isAncestorOf(widget);
    const bool inQueue = widget == m_queuePanel || m_queuePanel->isAncestorOf(widget);
    // Child mouse events can bubble to the backdrop. Do not let that parent
    // steal the transcript's focus and cancel the click it just received.
    const bool inDetail = m_playbackDetailVisible && widget == m_playbackDetailPage
        && !widget->childAt(position);
    // Transcript clicks and queue item selection keep their native mouse path.
    if (isPlaybackPanelBody(widget, m_subtitlePanel, m_queuePanel)) return false;
    if (const auto *view = qobject_cast<const QAbstractItemView *>(widget->parentWidget());
        view && widget == view->viewport()) {
        // An empty works-list viewport is part of the sidebar background;
        // clicking an actual work still selects it and retains list navigation.
        if ((inSidebar || m_libraryPanel->isTrackViewTarget(widget))
            && !view->indexAt(position).isValid())
            return true;
        return false;
    }
    if (!inSidebar && !inFooter && !inInformation && !inSubtitles && !inQueue && !inDetail) return false;
    if (widget == m_playbackControls || widget == m_subtitlePanel || widget == m_queuePanel) return true;
    if (widget == m_playbackControls->findChild<QSlider *>(QStringLiteral("seekSlider")))
        return false;
    return isPlaybackKeyboardTarget(widget);
}

bool MainWindow::isPlaybackKeyboardTarget(const QWidget *widget) const
{
    if (!widget || widget->window() != this || !widget->isEnabled()) return false;
    if (const auto *slider = m_playbackControls->findChild<QSlider *>(QStringLiteral("seekSlider"));
        widget == slider)
        return !slider->isSliderDown();
    if (isPlaybackPanelBody(widget, m_subtitlePanel, m_queuePanel)) return true;
    if (m_libraryPanel->isTrackViewTarget(widget)) return true;
    if (widget != this && widget->focusPolicy() != Qt::NoFocus) return false;
    // A passive child label must not turn its interactive owner into a
    // playback shortcut target. Other lists and scroll areas keep their keys.
    for (auto *parent = widget; parent && parent != this; parent = parent->parentWidget()) {
        if (qobject_cast<const QAbstractButton *>(parent)
            || qobject_cast<const QAbstractSlider *>(parent)
            || qobject_cast<const QAbstractItemView *>(parent)
            || qobject_cast<const QAbstractScrollArea *>(parent)
            || qobject_cast<const QAbstractSpinBox *>(parent)
            || qobject_cast<const QComboBox *>(parent)
            || qobject_cast<const QLineEdit *>(parent)
            || qobject_cast<const QTextEdit *>(parent)
            || qobject_cast<const QPlainTextEdit *>(parent)
            || qobject_cast<const QMenu *>(parent)
            || qobject_cast<const QMenuBar *>(parent))
            return false;
        if (const auto *label = qobject_cast<const QLabel *>(parent)) {
            const auto flags = label->textInteractionFlags();
            // QLabel defaults to mouse-accessible links even for PlainText.
            // Such presentation labels have no links to activate; selection
            // and actual/potential rich-text link interaction keep their keys.
            if (flags.testFlag(Qt::TextSelectableByMouse)
                || flags.testFlag(Qt::TextSelectableByKeyboard)
                || flags.testFlag(Qt::LinksAccessibleByKeyboard)
                || (flags.testFlag(Qt::LinksAccessibleByMouse)
                    && label->textFormat() != Qt::PlainText))
                return false;
        }
    }
    return true;
}

bool MainWindow::playbackKeyboardReady(bool seeking) const
{
    if (m_closing || !m_audioGeneration || m_audioPlayer->source().isEmpty()
        || m_progressSourceChanging || m_progressQueryPending || m_progressRestorePending
        || m_progressError || m_audioOpenPending || m_audioOpenInProgress
        || m_queuePreparationPending || m_queuePreparationInProgress
        || m_queueContinuationPending || m_queueNaturalRequest || m_queueReplacementPending
        || m_audioPlayer->error() != QMediaPlayer::NoError)
        return false;
    const auto status = m_audioPlayer->mediaStatus();
    if (status == QMediaPlayer::NoMedia || status == QMediaPlayer::LoadingMedia
        || status == QMediaPlayer::InvalidMedia)
        return false;
    const auto state = m_audioPlayer->playbackState();
    return !seeking || ((state == QMediaPlayer::PlayingState || state == QMediaPlayer::PausedState)
        && status != QMediaPlayer::EndOfMedia && m_audioPlayer->isSeekable()
        && m_audioPlayer->duration() > 0);
}

bool MainWindow::handlePlaybackKeyEvent(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease
        && event->type() != QEvent::ShortcutOverride)
        return false;
    const auto *key = static_cast<QKeyEvent *>(event);
    const bool space = key->key() == Qt::Key_Space;
    const bool backward = key->key() == Qt::Key_Left;
    if ((!space && !backward && key->key() != Qt::Key_Right)
        || key->modifiers() != Qt::NoModifier || QApplication::activePopupWidget()
        || QApplication::activeModalWidget()
        || (QApplication::activeWindow() && QApplication::activeWindow() != this))
        return false;
    const auto *target = qobject_cast<QWidget *>(watched);
    const auto *focus = QApplication::focusWidget();
    // Check the original focus too: an ignored key can propagate from an
    // editor/button to a passive parent, and must remain that control's key.
    if (!isPlaybackKeyboardTarget(target)
        || (focus && !isPlaybackKeyboardTarget(focus)))
        return false;
    // Dedicated playback keys remain owned during source/restore guards. This
    // also prevents a guarded Space from turning into a subtitle page scroll.
    if (!playbackKeyboardReady(!space)) {
        if (target != m_playbackControls->findChild<QSlider *>(QStringLiteral("seekSlider"))
            && !isPlaybackPanelBody(target, m_subtitlePanel, m_queuePanel))
            return false;
        event->accept();
        return true;
    }
    event->accept();
    if (event->type() == QEvent::KeyPress) {
        if (space) {
            if (!key->isAutoRepeat()) togglePlaybackFromControls();
        } else {
            seekPlaybackByKeyboard(backward ? -5000 : 5000);
        }
    }
    // The native play button itself is deliberately excluded above and keeps
    // its own single press/release activation. Passive areas consume Space's
    // release and repeated presses without toggling playback again.
    return true;
}

void MainWindow::clearKeyboardSeek()
{
    m_keyboardSeekTarget.reset();
    m_keyboardSeekSource.clear();
    m_keyboardSeekAudioGeneration = 0;
    m_keyboardSeekClock.invalidate();
    ++m_keyboardSeekRequestGeneration;
}

void MainWindow::seekPlaybackByKeyboard(qint64 offset)
{
    if (!playbackKeyboardReady(true)) return;
    constexpr int PendingSeekTimeoutMs = 1500;
    if (m_keyboardSeekTarget && (m_keyboardSeekAudioGeneration != m_audioGeneration
        || m_keyboardSeekSource != m_audioPlayer->source() || !m_keyboardSeekClock.isValid()
        || m_keyboardSeekClock.elapsed() >= PendingSeekTimeoutMs))
        clearKeyboardSeek();
    const auto audio = m_audioGeneration;
    const auto source = m_audioPlayer->source();
    // A paused seek stays inside the media's final valid millisecond. Seeking
    // to the exact endpoint can make the backend report EndOfMedia and falsely
    // finish progress or advance/repeat the queue without a play intention.
    const bool paused = m_audioPlayer->playbackState() == QMediaPlayer::PausedState;
    const qint64 duration = m_audioPlayer->duration() - (paused ? 1 : 0);
    const qint64 base = qBound<qint64>(0, m_keyboardSeekTarget.value_or(m_audioPlayer->position()), duration);
    // Avoid overflow even for unusually long media. Repeated key presses
    // accumulate from the last unconfirmed target, rather than stale time.
    const qint64 position = offset > 0 ? base + qMin(offset, duration - base)
                                     : base - qMin(-offset, base);
    seekPlaybackTo(position);
    if (audio != m_audioGeneration || source != m_audioPlayer->source()
        || !playbackKeyboardReady(true) || qAbs(m_audioPlayer->position() - position) <= 250)
        return;
    m_keyboardSeekTarget = position;
    m_keyboardSeekSource = source;
    m_keyboardSeekAudioGeneration = audio;
    m_keyboardSeekClock.start();
    const auto request = ++m_keyboardSeekRequestGeneration;
    QTimer::singleShot(PendingSeekTimeoutMs, this, [this, request] {
        if (request == m_keyboardSeekRequestGeneration) clearKeyboardSeek();
    });
}

void MainWindow::updatePlaybackQueue()
{
    m_queuePanel->setQueue(m_playbackQueue);
    m_playbackControls->setQueueState(static_cast<int>(m_playbackQueue.entries().size()),
                                     !m_queuePanel->isHidden());
    m_playbackControls->setQueueNavigationAvailable(!m_closing && m_playbackQueue.previousId() != 0,
                                                   !m_closing && m_playbackQueue.nextId() != 0);
    m_playbackControls->setPlaybackMode(m_playbackQueue.mode());
    updateFloatingPlaybackControls();
}

void MainWindow::cancelQueueTransition()
{
    clearKeyboardSeek();
    // User stop/play decisions supersede an already queued EndOfMedia turn
    // and any in-flight preparation/preflight, without moving the queue cursor.
    ++m_audioOpenGeneration;
    m_audioOpenPending = false;
    m_queuePreparationPending = false;
    m_queueAdvanceArmed = false;
    m_queueContinuationPending = false;
    m_queueNaturalRequest = false;
    m_queueRestartingCurrent = false;
    m_queueReplacementPending = false;
    m_queueAppendDuringReplacement.clear();
}

void MainWindow::playDirectory(qint64 workId, const QString &directory, bool recursive)
{
    if (m_closing) return;
    cancelQueueTransition();
    if (!m_libraryDatabaseReady || m_libraryQueryInProgress || m_libraryQueryPending
        || workId <= 0 || workId != m_librarySnapshotWorkId
        || workId != m_libraryPanel->selectedWorkId()
        || directory != m_libraryPanel->currentDirectory()) {
        showAudioOpenError(QStringLiteral("目录快照尚未就绪或已更新，请重新选择。"));
        return;
    }
    m_queuePreparationRequest = {};
    m_queuePreparationRequest.kind = QueuePreparationKind::Directory;
    m_queuePreparationRequest.generation = m_audioOpenGeneration;
    m_queuePreparationRequest.workId = workId;
    m_queuePreparationRequest.directory = directory;
    m_queuePreparationRequest.recursive = recursive;
    m_queuePreparationRequest.replacement = m_playbackQueue;
    m_queueReplacementPending = true;
    m_queuePreparationPending = true;
    if (!m_queuePreparationInProgress) startQueuePreparation();
}

void MainWindow::enqueueLibraryTrack(qint64 fileId)
{
    enqueueLibraryTracks(m_librarySnapshotWorkId, {fileId});
}

void MainWindow::enqueueLibraryTracks(qint64 workId, const QVector<qint64> &fileIds)
{
    if (m_closing || !m_libraryDatabaseReady || m_libraryQueryInProgress || m_libraryQueryPending
        || fileIds.isEmpty() || workId <= 0 || workId != m_librarySnapshotWorkId
        || workId != m_libraryPanel->selectedWorkId()) return;
    const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &item) { return item.id == workId; });
    if (work == m_libraryWorksSnapshot.cend() || work->rootPath.isEmpty()
        || m_removedLibraryWorkIds.contains(workId)) return;
    QVector<PlaybackQueueEntry> entries;
    QSet<qint64> seen;
    // Validate the complete frozen selection before changing either queue.
    // Playback retains its asynchronous filesystem and identity checks.
    for (const auto fileId : fileIds) {
        if (seen.contains(fileId)) continue;
        const auto file = std::find_if(m_libraryFilesSnapshot.cbegin(), m_libraryFilesSnapshot.cend(),
            [fileId](const auto &item) { return item.id == fileId; });
        if (fileId <= 0 || file == m_libraryFilesSnapshot.cend() || file->workId != workId
            || file->type != LibraryFileType::Audio || file->missing) return;
        const auto relative = QDir::fromNativeSeparators(file->relativePath);
        const auto parts = relative.split(QLatin1Char('/'));
        if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(QLatin1Char(':'))
            || relative.contains(QChar::Null) || parts.contains(QStringLiteral(".."))
            || parts.contains(QStringLiteral(".")) || parts.contains(QString())) return;
        seen.insert(fileId);
        PlaybackQueueEntry entry;
        entry.workId = workId; entry.fileId = fileId;
        entry.rootPath = work->rootPath; entry.relativePath = file->relativePath;
        entry.workName = work->displayName; entry.displayName = file->fileName;
        entries.append(std::move(entry));
    }
    const auto added = m_playbackQueue.append(entries);
    if (m_queueReplacementPending) m_queueAppendDuringReplacement.append(entries);
    updatePlaybackQueue();
    m_queuePanel->setToolTip(added ? QStringLiteral("已加入 %1 首音轨到播放队列").arg(added)
                                 : QStringLiteral("所选音轨已在队列中"));
}

void MainWindow::playQueueEntry(quint64 entryId, PlaybackQueueNavigation navigation)
{
    if (m_closing || !entryId) return;
    cancelQueueTransition();
    const auto *entry = m_playbackQueue.entry(entryId);
    if (!entry || !entry->available) return;
    m_queueNaturalRequest = navigation == PlaybackQueueNavigation::Natural;
    m_queuePreparationRequest = {};
    m_queuePreparationRequest.kind = QueuePreparationKind::Entry;
    m_queuePreparationRequest.generation = m_audioOpenGeneration;
    m_queuePreparationRequest.workId = entry->workId;
    m_queuePreparationRequest.entryId = entryId;
    m_queuePreparationRequest.entry = *entry;
    m_queuePreparationRequest.navigation = navigation;
    m_queuePreparationPending = true;
    if (!m_queuePreparationInProgress) startQueuePreparation();
}

void MainWindow::startQueuePreparation()
{
    m_queuePreparationInProgress = true;
    m_queuePreparationPending = false;
    updateFloatingPlaybackControls();
    const auto request = m_queuePreparationRequest;
    const QString database = m_libraryDatabasePath;
    auto promise = std::make_shared<QPromise<QueuePreparationResult>>();
    auto *watcher = new QFutureWatcher<QueuePreparationResult>(this);
    connect(watcher, &QFutureWatcher<QueuePreparationResult>::finished, this, [this, watcher, request] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_queuePreparationInProgress = false;
        if (!m_closing && request.generation == m_audioOpenGeneration) {
            if (m_removedLibraryWorkIds.contains(request.workId)) {
                failQueueRequest(request.entryId, QStringLiteral("作品记录已清理。"), true);
            } else if (!result.success) {
                failQueueRequest(request.entryId, result.error, result.unavailable);
                showAudioOpenError(result.error);
            } else {
                auto replacement = result.replacement;
                auto entryId = result.entryId;
                if (replacement) {
                    const auto startIndex = replacement->currentIndex();
                    replacement = m_playbackQueue.replacementFor(replacement->entries(), startIndex);
                    entryId = replacement->currentId();
                }
                requestOpenAudio(result.entry.relativePath, result.entry.rootPath,
                    result.entry.workId, result.entry.fileId, result.entry.workName,
                    entryId, replacement, request.navigation,
                    request.navigation == PlaybackQueueNavigation::Natural
                        && m_completedAudioIdentities.contains(playbackProgressIdentity(
                            result.entry.workId, result.entry.fileId,
                            QDir(result.entry.rootPath).filePath(result.entry.relativePath))));
            }
        }
        if (m_closing) finishCloseIfReady();
        else if (m_queuePreparationPending) startQueuePreparation();
        updateFloatingPlaybackControls();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, request, database] {
        promise->start();
        QueuePreparationResult result;
        try {
            if (request.kind == QueuePreparationKind::Entry && request.workId == 0) {
                result.entry = request.entry;
                result.entryId = request.entryId;
                result.success = true;
            } else {
                LibraryRepository repository(database);
                const auto opened = repository.open();
                if (!opened.success) result.error = diagnostics(opened);
                else {
                    const auto snapshot = repository.workSnapshot(request.workId);
                    if (!snapshot.success) {
                        result.error = diagnostics(snapshot);
                        result.unavailable = std::any_of(snapshot.diagnostics.cbegin(), snapshot.diagnostics.cend(),
                            [](const auto &item) { return item.code == LibraryDiagnosticCode::InvalidWorkSnapshot; });
                    } else if (request.kind == QueuePreparationKind::Directory) {
                        auto entries = buildDirectoryPlaybackEntries(snapshot.work, snapshot.files,
                                                                      request.directory, request.recursive);
                        int startIndex = 0;
                        if (request.startFileId > 0) {
                            const auto target = std::find_if(entries.cbegin(), entries.cend(),
                                [&request](const auto &item) {
                                    return item.fileId == request.startFileId
                                        && item.relativePath == request.entry.relativePath;
                                });
                            startIndex = target == entries.cend() ? -1
                                : static_cast<int>(std::distance(entries.cbegin(), target));
                        }
                        if (startIndex < 0)
                            result.error = QStringLiteral("所选音轨已缺失或不再属于原目录，原播放和队列保持。");
                        else if (entries.isEmpty()) result.error = QStringLiteral("此范围没有未标记缺失的音轨，原播放和队列保持。 ");
                        else {
                            result.replacement = request.replacement;
                            result.replacement->replace(std::move(entries), startIndex);
                            result.entry = *result.replacement->current();
                            result.entryId = result.entry.id;
                            result.success = true;
                        }
                    } else {
                        const auto file = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(),
                            [&request](const auto &item) { return item.id == request.entry.fileId
                                && item.workId == request.workId && item.type == LibraryFileType::Audio; });
                        if (file == snapshot.files.cend() || file->missing) {
                            result.error = QStringLiteral("队列音轨已缺失或不再属于原作品：%1").arg(request.entry.relativePath);
                            result.unavailable = true;
                        } else {
                            result.entry = request.entry;
                            result.entry.rootPath = snapshot.work.rootPath;
                            result.entry.workName = snapshot.work.displayName;
                            result.entry.relativePath = file->relativePath;
                            result.entry.displayName = file->fileName;
                            result.entryId = request.entryId;
                            result.success = true;
                        }
                    }
                }
            } // Worker-owned repository is destroyed before publishing values.
        } catch (const std::exception &exception) {
            result.error = QStringLiteral("准备播放队列失败：%1").arg(QString::fromUtf8(exception.what()));
        } catch (...) { result.error = QStringLiteral("准备播放队列发生未知错误。"); }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void MainWindow::adoptPlaybackQueue(const QString &path, qint64 workId, qint64 fileId,
    const QString &workName, quint64 entryId, const std::optional<PlaybackQueue> &replacement,
    PlaybackQueueNavigation navigation)
{
    if (replacement) {
        auto accepted = *replacement;
        // A directory candidate may have been prepared before the user changed
        // mode. Its media entries are accepted with the current session mode.
        accepted.setMode(m_playbackQueue.mode());
        m_playbackQueue = accepted;
    }
    if (entryId) {
        // A replacement already selected its initial entry. Re-selecting it
        // would create a second shuffle history observation.
        if (!replacement || m_playbackQueue.currentId() != entryId)
            m_playbackQueue.setCurrent(entryId, navigation);
        m_playbackQueue.setIssue(entryId, {});
    } else {
        PlaybackQueueEntry entry;
        entry.workId = workId; entry.fileId = fileId; entry.workName = workName;
        entry.rootPath = workId > 0 ? m_audioRequestedRoot : QString();
        entry.relativePath = entry.rootPath.isEmpty() ? path : QDir(entry.rootPath).relativeFilePath(path);
        entry.displayName = QFileInfo(path).fileName();
        // An explicitly opened file outside the browser retains a single-track
        // queue; library activation supplies a directory replacement instead.
        m_playbackQueue.replace({entry});
    }
    m_playbackQueue.append(m_queueAppendDuringReplacement);
    // Pending preparations may have captured names before an editor commit.
    // Reconcile presentation metadata without replacing queue identities.
    syncQueueWorkNames(m_playbackQueue);
    for (const auto removedWork : m_removedLibraryWorkIds) m_playbackQueue.invalidateWork(removedWork);
    m_queueAppendDuringReplacement.clear();
    m_queueReplacementPending = false;
    m_queueNaturalRequest = false;
    m_queueContinuationPending = false;
    m_queueRestartingCurrent = false;
    m_queueSource = m_audioPlayer->source();
    m_queueAudioGeneration = m_audioGeneration;
    m_queueEndedGeneration = 0;
    m_queueAdvanceArmed = m_audioPlayer->error() == QMediaPlayer::NoError
        && m_audioPlayer->mediaStatus() != QMediaPlayer::InvalidMedia;
    if (!m_queueAdvanceArmed)
        m_playbackQueue.setIssue(m_playbackQueue.currentId(), QStringLiteral("音频解码失败"));
    updatePlaybackQueue();
}

void MainWindow::failQueueRequest(quint64 entryId, const QString &message, bool unavailable)
{
    m_queueAdvanceArmed = false;
    m_queueNaturalRequest = false;
    m_queueContinuationPending = false;
    m_queueReplacementPending = false;
    m_queueAppendDuringReplacement.clear();
    if (entryId) m_playbackQueue.setIssue(entryId, message, unavailable);
    updatePlaybackQueue();
}

void MainWindow::removeQueueEntry(quint64 entryId)
{
    if (m_closing || entryId == m_playbackQueue.currentId()) return;
    if (const auto *entry = m_playbackQueue.entry(entryId)) {
        const auto workId = entry->workId;
        const auto fileId = entry->fileId;
        m_queueAppendDuringReplacement.removeIf([workId, fileId](const auto &item) {
            return item.workId == workId && item.fileId == fileId;
        });
    }
    if (m_audioRequestedQueueEntryId == entryId
        || (m_queuePreparationInProgress && m_queuePreparationRequest.entryId == entryId)) {
        const bool playingSession = m_queueSource == m_audioPlayer->source()
            && m_queueAudioGeneration == m_audioGeneration
            && m_audioPlayer->playbackState() != QMediaPlayer::StoppedState
            && m_audioPlayer->error() == QMediaPlayer::NoError;
        cancelQueueTransition();
        m_queueAdvanceArmed = playingSession;
    }
    m_playbackQueue.remove(entryId);
    updatePlaybackQueue();
}

void MainWindow::clearOtherQueueEntries()
{
    if (m_closing) return;
    const bool playingSession = m_queueSource == m_audioPlayer->source()
        && m_queueAudioGeneration == m_audioGeneration
        && m_audioPlayer->playbackState() != QMediaPlayer::StoppedState
        && m_audioPlayer->error() == QMediaPlayer::NoError;
    cancelQueueTransition();
    m_queueAdvanceArmed = playingSession;
    m_playbackQueue.clearOthers();
    updatePlaybackQueue();
}

void MainWindow::onQueueMediaStatus(QMediaPlayer::MediaStatus status)
{
    if (status != QMediaPlayer::EndOfMedia) {
        if (m_queueRestartingCurrent && m_queueSource == m_audioPlayer->source()
            && m_queueAudioGeneration == m_audioGeneration
            && m_audioPlayer->mediaStatus() != QMediaPlayer::EndOfMedia) {
            m_queueRestartingCurrent = false;
            m_queueEndedGeneration = 0;
        }
        return;
    }
    if (!m_closing && !m_progressSourceChanging && m_queueAudioGeneration == m_audioGeneration
        && m_audioPlayer->mediaStatus() == status && m_audioPlayer->error() == QMediaPlayer::NoError
        && m_audioPlayer->source() == m_queueSource && m_playbackQueue.currentId())
        m_completedAudioIdentities.insert(playbackProgressIdentity(m_playingLibraryWorkId,
            m_playingLibraryFileId, m_audioPlayer->source().toLocalFile()));
    if (status != QMediaPlayer::EndOfMedia || m_closing || m_progressSourceChanging
        || !m_queueAdvanceArmed || m_queueAudioGeneration != m_audioGeneration
        || m_queueEndedGeneration == m_audioGeneration || !m_playbackQueue.currentId()
        || m_audioPlayer->mediaStatus() != status || m_audioPlayer->error() != QMediaPlayer::NoError
        || m_audioPlayer->source() != m_queueSource) return;
    m_queueEndedGeneration = m_audioGeneration;
    scheduleQueueContinuation();
}

void MainWindow::setPlaybackMode(PlaybackMode mode)
{
    if (m_closing || mode == m_playbackQueue.mode()) return;
    const bool reviseNaturalRequest = m_queueNaturalRequest || m_queueContinuationPending;
    m_playbackQueue.setMode(mode);
    updatePlaybackQueue();
    // Explicit queue selections and directory replacement requests retain
    // their target. Only an unaccepted automatic continuation is re-decided.
    if (!reviseNaturalRequest) return;
    cancelQueueTransition();
    if (m_queueSource != m_audioPlayer->source() || m_queueAudioGeneration != m_audioGeneration
        || m_audioPlayer->mediaStatus() != QMediaPlayer::EndOfMedia
        || m_audioPlayer->error() != QMediaPlayer::NoError) return;
    m_queueAdvanceArmed = true;
    scheduleQueueContinuation();
}

QString MainWindow::playbackProgressIdentity(qint64 workId, qint64 fileId, const QString &path) const
{
    if (workId > 0 && fileId > 0)
        return QStringLiteral("library:%1:%2").arg(workId).arg(fileId);
    return QStringLiteral("temporary:") + QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

void MainWindow::scheduleQueueContinuation()
{
    const auto audio = m_audioGeneration;
    const auto intent = m_audioOpenGeneration;
    const auto current = m_playbackQueue.currentId();
    const auto source = m_queueSource;
    m_queueContinuationPending = true;
    // onProgressMediaStatus has already queued the finished track's clear.
    QTimer::singleShot(0, this, [this, audio, intent, current, source] {
        if (intent != m_audioOpenGeneration) return;
        m_queueContinuationPending = false;
        if (m_closing || !m_queueAdvanceArmed || audio != m_audioGeneration
            || current != m_playbackQueue.currentId() || source != m_audioPlayer->source()
            || m_audioPlayer->error() != QMediaPlayer::NoError
            || m_audioPlayer->mediaStatus() != QMediaPlayer::EndOfMedia) return;
        const auto next = m_playbackQueue.naturalNextId();
        if (!next) {
            m_queueAdvanceArmed = false;
            return;
        }
        if (next == current) restartCurrentQueueEntry();
        else playQueueEntry(next, PlaybackQueueNavigation::Natural);
    });
}

void MainWindow::restartCurrentQueueEntry()
{
    const auto *entry = m_playbackQueue.current();
    if (m_closing || !entry || !entry->available || !m_queueAdvanceArmed
        || m_queueSource != m_audioPlayer->source() || m_queueAudioGeneration != m_audioGeneration
        || m_audioPlayer->error() != QMediaPlayer::NoError
        || m_audioPlayer->mediaStatus() != QMediaPlayer::EndOfMedia) return;
    cancelQueueTransition();
    cancelPlaybackRestore(false);
    m_queueAdvanceArmed = true;
    m_queueRestartingCurrent = true;
    // Keep the accepted source, cover, parsed subtitles and current selection.
    // The natural-end clear remains queued (and its error remains visible).
    // Seeking precedes play so a failed clear cannot restore the old endpoint.
    m_audioPlayer->setPosition(0);
    m_audioPlayer->play();
    if (m_audioPlayer->mediaStatus() != QMediaPlayer::EndOfMedia) {
        m_queueRestartingCurrent = false;
        m_queueEndedGeneration = 0;
    }
    refreshSubtitles();
    updatePlaybackQueue();
}
} // namespace asmr
