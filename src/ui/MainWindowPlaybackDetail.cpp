#include "ui/MainWindow.h"

#include "playback/AudioPlayer.h"
#include "ui/LibraryPanel.h"
#include "ui/PlaybackControlsWidget.h"
#include "ui/PlaybackDetailWidget.h"
#include "ui/PlaybackDetailTransition.h"
#include "ui/SubtitlePanel.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolTip>
#include <QVBoxLayout>

namespace asmr {
void MainWindow::updatePlaybackIdentity(const QString &audioName, const QString &workName)
{
    m_playingAudioName = audioName;
    m_playingWorkName = workName;
    m_playbackControls->setAudioName(audioName);
    m_playbackControls->setWorkName(workName);
}

void MainWindow::setPlaybackDetailVisible(bool visible)
{
    if (m_closing || m_playbackDetailTransitionPreparing || visible == m_playbackDetailVisible) return;
    if (visible && (!m_audioGeneration || m_audioPlayer->source().isEmpty()
        || QApplication::activePopupWidget() || QApplication::activeModalWidget())) return;

    const bool continuing = m_playbackDetailTransition->isVisible()
        && m_playbackDetailTransition->hasSnapshots();
    const bool animate = continuing || (isVisible() && !isMinimized()
        && m_contentStack->isVisible() && !m_contentStack->size().isEmpty());
    QPixmap previous;
    {
        QScopedValueRollback<bool> preparing(m_playbackDetailTransitionPreparing, true);
        showPlaybackQueue(false);
        m_subtitlePanel->cancelPendingSeek();
        QToolTip::hideText();
        if (visible && !continuing) {
            // Capture once for the whole reversible chain. An intervening close
            // must not replace this with the temporary main-window focus.
            m_playbackDetailReturnFocus = QApplication::focusWidget();
            m_savedWorkspaceSizes = m_workspaceSplitter->sizes();
        }
        auto *coverButton = m_playbackControls->findChild<QPushButton *>(
            QStringLiteral("togglePlaybackDetailButton"));
        const auto rememberControls = [&] {
            const QRect cover(coverButton->mapTo(m_contentStack, QPoint()), coverButton->size());
            if (m_playbackDetailVisible) {
                m_playbackDetailExpandedCoverRect = cover;
                m_playbackDetailCollapseRect = QRect(
                    m_collapsePlaybackDetailButton->mapTo(m_contentStack, QPoint()),
                    m_collapsePlaybackDetailButton->size());
            } else {
                m_playbackDetailLibraryCoverRect = cover;
            }
        };
        m_playbackDetailTransition->cancel();
        m_playbackDetailTransition->hide();
        if (animate && !continuing) {
            synchronizePlaybackDetailLayout();
            rememberControls();
            previous = m_contentStack->grab();
        }
        applyPlaybackDetailLayout(visible);
        if (visible) {
            m_playbackDetailTransitionEndFocus = m_subtitlePanel;
        } else if (m_playbackDetailReturnFocus && m_playbackDetailReturnFocus->isVisible()
                   && m_playbackDetailReturnFocus->isEnabled()) {
            m_playbackDetailTransitionEndFocus = m_playbackDetailReturnFocus;
        } else {
            m_playbackDetailTransitionEndFocus = m_playbackControls;
        }
        if (animate && !continuing) {
            rememberControls();
            const QPixmap destination = m_contentStack->grab();
            const bool snapshotsReady = visible
                ? m_playbackDetailTransition->setSnapshots(previous, destination)
                : m_playbackDetailTransition->setSnapshots(destination, previous);
            if (snapshotsReady) m_playbackDetailTransition->snapTo(!visible);
        }
    }
    if (!animate || !m_playbackDetailTransition->hasSnapshots()) {
        finishPlaybackDetailTransition();
        return;
    }
    m_playbackDetailPointerAction = 0;
    m_playbackDetailTransition->setGeometry(m_contentStack->rect());
    m_playbackDetailTransition->show();
    m_playbackDetailTransition->raise();
    // The snapshots have no interactive children. Keep playback keys on a
    // passive real target until the desired page receives its final focus.
    setFocus(Qt::OtherFocusReason);
    m_playbackDetailTransition->animateTo(visible);
}

void MainWindow::applyPlaybackDetailLayout(bool visible)
{
    m_playbackDetailVisible = visible;
    if (visible) {
        (void)m_libraryPanel->takeWorkspaceFooter();
        m_subtitlePanel->setMinimumWidth(0);
        m_subtitlePanel->setDetailMode(true);
        m_playbackDetailLayout->addWidget(m_subtitlePanel, 1);
        m_playbackDetailLayout->addWidget(m_playbackFooter);
        m_contentStack->setCurrentWidget(m_playbackDetailPage);
        m_subtitlePanel->show();
        m_playbackFooter->show();
        m_playbackControls->setPlaybackDetailVisible(true);
        setTabOrder(m_collapsePlaybackDetailButton, m_subtitlePanel);
        setTabOrder(m_subtitlePanel, m_playbackControls);
    } else {
        m_playbackDetailLayout->removeWidget(m_subtitlePanel);
        m_playbackDetailLayout->removeWidget(m_playbackFooter);
        m_workspaceSplitter->addWidget(m_subtitlePanel);
        m_subtitlePanel->setDetailMode(false);
        m_subtitlePanel->setMinimumWidth(260);
        m_libraryPanel->setWorkspaceFooter(m_playbackFooter);
        m_contentStack->setCurrentWidget(m_libraryPanel);
        m_playbackControls->setPlaybackDetailVisible(false);
        applySubtitlePanelVisibility(!m_compactLayout && m_subtitlePanelRequested);
        m_workspaceSplitter->setSizes(m_savedWorkspaceSizes);
        m_playbackFooter->show();
        configureTabOrder();
    }
    synchronizePlaybackDetailLayout();
    updateQueuePanelGeometry();
}

void MainWindow::synchronizePlaybackDetailLayout()
{
    // A child resize can update the artwork's preferred size. Settle both
    // nested layout passes before taking either endpoint, without pumping input.
    for (int pass = 0; pass < 2; ++pass) {
        m_contentStack->parentWidget()->layout()->activate();
        m_contentStack->layout()->activate();
        if (m_playbackDetailVisible) {
            m_playbackDetailLayout->activate();
        } else {
            m_libraryPanel->layout()->activate();
            m_workspaceSplitter->parentWidget()->layout()->activate();
            m_workspaceSplitter->setSizes(m_savedWorkspaceSizes);
            m_libraryPanel->prepareForSnapshot();
        }
        m_playbackFooter->layout()->activate();
        m_playbackControls->layout()->activate();
        m_subtitlePanel->layout()->activate();
    }
    m_subtitlePanel->prepareForSnapshot();
}

void MainWindow::finishPlaybackDetailTransition(bool restoreFocus)
{
    if (!m_playbackDetailTransition || m_playbackDetailTransitionPreparing) return;
    m_playbackDetailTransition->clearSnapshots();
    m_playbackDetailPointerAction = 0;
    if (restoreFocus && !m_closing && isVisible()
        && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()
        && (!QApplication::activeWindow() || QApplication::activeWindow() == this)
        && m_playbackDetailTransitionEndFocus && m_playbackDetailTransitionEndFocus->isVisible()
        && m_playbackDetailTransitionEndFocus->isEnabled())
        m_playbackDetailTransitionEndFocus->setFocus(Qt::OtherFocusReason);
    m_playbackDetailTransitionEndFocus.clear();
    if (!m_playbackDetailVisible) m_playbackDetailReturnFocus.clear();
}

bool MainWindow::handlePlaybackDetailTransitionEvent(QObject *watched, QEvent *event)
{
    if (!m_playbackDetailTransition || m_playbackDetailTransitionPreparing
        || !m_playbackDetailTransition->isVisible()) return false;
    const auto *widget = qobject_cast<QWidget *>(watched);
    const auto type = event->type();
    const bool mainLifecycle = watched == this
        && (type == QEvent::Hide || type == QEvent::Resize || type == QEvent::WindowStateChange
            || type == QEvent::WindowBlocked || type == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
            || type == QEvent::DevicePixelRatioChange
#endif
        );
    const bool stackResize = watched == m_contentStack && type == QEvent::Resize;
    const bool popupOpening = widget && widget->isWindow() && type == QEvent::Show
        && (widget->windowType() == Qt::Popup || widget->windowModality() != Qt::NonModal);
    if (mainLifecycle || stackResize || popupOpening) {
        finishPlaybackDetailTransition(false);
        return false;
    }
    if (!widget || widget->window() != this) return false;
    if (type == QEvent::ShortcutOverride || type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (QApplication::activePopupWidget() || QApplication::activeModalWidget()
            || (QApplication::activeWindow() && QApplication::activeWindow() != this)) return false;
        const bool tab = key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab;
        if (tab) {
            const auto *focus = QApplication::focusWidget();
            // Visible title-bar controls keep their own keyboard behavior.
            if (focus && focus != this && !m_contentStack->isAncestorOf(focus)) return false;
            event->accept();
            return true;
        }
        // Space and arrows use the existing playback-key filter, including
        // its native-button focus exclusions and media/seek guards.
        if (key->modifiers() != Qt::NoModifier || key->key() != Qt::Key_Escape) return false;
        event->accept();
        if (type == QEvent::KeyPress && !key->isAutoRepeat()) setPlaybackDetailVisible(false);
        return true;
    }
    const bool inContent = widget == m_contentStack || m_contentStack->isAncestorOf(widget);
    if (!inContent) return false;
    if (type == QEvent::Wheel || type == QEvent::ContextMenu || type == QEvent::ToolTip
        || type == QEvent::MouseMove) {
        event->accept();
        return true;
    }
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease
        && type != QEvent::MouseButtonDblClick) return false;
    const auto *mouse = static_cast<QMouseEvent *>(event);
    const QPoint point = m_contentStack->mapFromGlobal(mouse->globalPosition().toPoint());
    const int offset = qRound((1.0 - m_playbackDetailTransition->progress()) * 28.0);
    const auto hitAction = [&] {
        const qreal progress = m_playbackDetailTransition->progress();
        if ((progress < 1.0 && m_playbackDetailLibraryCoverRect.contains(point))
            || (progress > 0.0 && m_playbackDetailExpandedCoverRect.translated(0, offset).contains(point))) return 1;
        if (progress > 0.0 && m_playbackDetailCollapseRect.translated(0, offset).contains(point)) return 2;
        return 0;
    };
    if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier) {
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
            m_playbackDetailPointerAction = hitAction();
        } else {
            const int action = m_playbackDetailPointerAction;
            m_playbackDetailPointerAction = 0;
            if (action && action == hitAction())
                setPlaybackDetailVisible(action == 1 ? !m_playbackDetailVisible : false);
        }
    } else {
        m_playbackDetailPointerAction = 0;
    }
    event->accept();
    return true;
}

} // namespace asmr
