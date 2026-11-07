#pragma once

#include "ui/FloatingSubtitleSettings.h"
#include "subtitle/SubtitleEntry.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointF>
#include <QPointer>
#include <QWidget>

#include <memory>

class QCloseEvent;
class QAbstractButton;
class QEvent;
class QHideEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;
class QScrollArea;
class QTimer;
class QWindow;

namespace asmr {

// An unparented top-level subtitle window; its caller owns its lifetime.
class FloatingSubtitleWindow final : public QWidget
{
    Q_OBJECT

public:
    FloatingSubtitleWindow();
    ~FloatingSubtitleWindow() override;

    void setSubtitleText(const QString &text);
    void clearText();
    [[nodiscard]] QString subtitleText() const;
    void showWindow();
    void hideWindow();
    void setLocked(bool locked);
    [[nodiscard]] bool isLocked() const;
    void setAppearance(const FloatingSubtitleAppearance &appearance);
    [[nodiscard]] FloatingSubtitleAppearance appearance() const;
    void restorePosition(std::optional<QPoint> savedPosition);
    void restoreSize(std::optional<QSize> savedSize);
    void setPlaybackControlsState(bool playing, bool canToggle, bool canSeek,
                                  bool canPrevious, bool canNext);
    void setSubtitlePlaybackContext(const QList<SubtitleEntry> &entries,
                                    qint64 positionMs, bool playing);
    void resetAutoScroll();

signals:
    void visibilityChanged(bool visible);
    void lockedChanged(bool locked);
    void appearanceRequested();
    void appearanceChanged(const FloatingSubtitleAppearance &appearance);
    void previousRequested();
    void playPauseRequested();
    void nextRequested();
    void seekByRequested(qint64 offset);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshScreenConnections();
    void observeWindowScreen();
    void queueScreenLayoutCheck();
    void ensurePositionOnScreens();
    void updateControlPositions();
    void updateHoverControls();
    void updateTextLayout();
    void updateAutoScroll();
    void takeManualScrollControl();
    [[nodiscard]] Qt::Edges resizeEdgesAt(QPoint localPosition) const;
    void updateResizeCursor(QPoint localPosition);
    [[nodiscard]] int availableMaximumHeight() const;
    void applyResize(QPoint globalPosition);

    QLabel *m_textLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_toolbar = nullptr;
    QAbstractButton *m_previousButton = nullptr;
    QAbstractButton *m_playPauseButton = nullptr;
    QAbstractButton *m_nextButton = nullptr;
    QAbstractButton *m_backwardButton = nullptr;
    QAbstractButton *m_forwardButton = nullptr;
    // This tool must not share the input-transparent native subtitle window.
    std::unique_ptr<QWidget> m_unlockControl;
    QTimer *m_hoverTimer = nullptr;
    QTimer *m_autoScrollTimer = nullptr;
    QList<SubtitleEntry> m_autoScrollEntries;
    QElapsedTimer m_playbackSnapshotClock;
    qint64 m_playbackPositionMs = 0;
    qint64 m_autoScrollAnchorMs = 0;
    qint64 m_autoScrollEndMs = 0;
    bool m_playing = false;
    bool m_manualScroll = false;
    bool m_autoScrollNeedsAnchor = true;
    FloatingSubtitleAppearance m_appearance;
    QPointF m_dragOffset;
    bool m_dragging = false;
    Qt::Edges m_resizeEdges;
    QRect m_resizeStartGeometry;
    QPoint m_resizeStartPosition;
    int m_resizeMaximumHeight = MaximumFloatingSubtitleHeight;
    bool m_reflowing = false;
    bool m_locked = false;
    bool m_changingWindowFlags = false;
    bool m_correctingPosition = false;
    bool m_screenConnectionsDirty = true;
    bool m_screenLayoutCheckQueued = false;
    bool m_textLayoutUpdateQueued = false;
    QList<QMetaObject::Connection> m_screenConnections;
    QMetaObject::Connection m_windowScreenConnection;
    QPointer<QWindow> m_observedWindow;
};

} // namespace asmr
