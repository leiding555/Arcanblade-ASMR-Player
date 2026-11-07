#include "ui/FloatingSubtitleWindow.h"

#include <QAbstractButton>
#include <QCloseEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QEvent>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace asmr {
namespace {

enum class ControlIcon { Previous, Play, Pause, Next, Backward, Forward, Lock, Appearance, Close, Unlock };

bool sameSubtitleEntries(const QList<SubtitleEntry> &left, const QList<SubtitleEntry> &right)
{
    return left.size() == right.size()
        && std::equal(left.cbegin(), left.cend(), right.cbegin(),
            [](const SubtitleEntry &a, const SubtitleEntry &b) {
                return a.startMs == b.startMs && a.endMs == b.endMs && a.text == b.text;
            });
}

QList<SubtitleEntry> sortedSubtitleEntries(QList<SubtitleEntry> entries)
{
    // Treat overlapping cues as a collection, independent of presentation order.
    std::sort(entries.begin(), entries.end(), [](const SubtitleEntry &a, const SubtitleEntry &b) {
        if (a.startMs != b.startMs)
            return a.startMs < b.startMs;
        if (a.endMs != b.endMs)
            return a.endMs < b.endMs;
        return a.text < b.text;
    });
    return entries;
}

class SubtitleTextLabel final : public QLabel
{
public:
    using QLabel::QLabel;

    int heightForWidth(int width) const override
    {
        if (text().isEmpty())
            return 0;
        return fontMetrics().boundingRect(QRect(0, 0, qMax(1, width), 1'000'000),
                                          TextFlags, text()).height();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // QLabel's default word wrap can leave a long unbroken token wider
        // than its viewport. Measure and draw literal text with identical
        // wrap-anywhere flags, retaining all characters at the chosen font.
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(), TextFlags | Qt::AlignVCenter, text());
    }

private:
    static constexpr int TextFlags = Qt::TextWordWrap | Qt::TextWrapAnywhere | Qt::AlignHCenter;
};

// Painted controls keep the symbols consistent without relying on an icon font.
class SubtitleControlButton final : public QAbstractButton
{
public:
    SubtitleControlButton(ControlIcon icon, const QString &label, QWidget *parent)
        : QAbstractButton(parent), m_icon(icon)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(label);
        setToolTip(label);
        setFixedSize(icon == ControlIcon::Unlock ? QSize(96, 34) : QSize(32, 32));
        if (icon == ControlIcon::Unlock)
            setText(label);
    }

    void setIcon(ControlIcon icon)
    {
        m_icon = icon;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (isEnabled() && (isDown() || underMouse())) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(isDown() ? QColor(255, 255, 255, 42)
                                     : QColor(255, 255, 255, 24));
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1),
                                    m_icon == ControlIcon::Unlock ? 16 : 6,
                                    m_icon == ControlIcon::Unlock ? 16 : 6);
        }
        painter.save();
        painter.translate(m_icon == ControlIcon::Unlock ? 24.0 : width() / 2.0,
                          height() / 2.0);
        const QColor foreground(242, 244, 243, isEnabled() ? 255 : 82);
        painter.setPen(QPen(foreground, 1.7, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (m_icon == ControlIcon::Play || m_icon == ControlIcon::Pause) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(foreground);
            if (m_icon == ControlIcon::Play) {
                painter.drawPolygon(QPolygonF{QPointF(-4, -7), QPointF(7, 0), QPointF(-4, 7)});
            } else {
                painter.drawRoundedRect(QRectF(-6, -7, 4, 14), 1, 1);
                painter.drawRoundedRect(QRectF(2, -7, 4, 14), 1, 1);
            }
        } else if (m_icon == ControlIcon::Previous || m_icon == ControlIcon::Next) {
            const qreal direction = m_icon == ControlIcon::Next ? 1 : -1;
            painter.setBrush(foreground);
            painter.drawPolygon(QPolygonF{QPointF(-4 * direction, -6), QPointF(4 * direction, 0),
                                           QPointF(-4 * direction, 6)});
            painter.drawLine(QPointF(7 * direction, -6), QPointF(7 * direction, 6));
        } else if (m_icon == ControlIcon::Backward || m_icon == ControlIcon::Forward) {
            const qreal direction = m_icon == ControlIcon::Forward ? 1 : -1;
            painter.setPen(Qt::NoPen);
            painter.setBrush(foreground);
            for (const qreal x : {-4.0, 3.0}) {
                painter.drawPolygon(QPolygonF{QPointF((x - 3) * direction, -7),
                    QPointF((x + 3) * direction, -2), QPointF((x - 3) * direction, 3)});
            }
            QFont secondsFont = font();
            secondsFont.setPixelSize(9);
            secondsFont.setBold(true);
            painter.setFont(secondsFont);
            painter.setPen(foreground);
            painter.drawText(QRectF(-7, 3, 14, 10), Qt::AlignCenter, QStringLiteral("5"));
        } else if (m_icon == ControlIcon::Close) {
            painter.drawLine(QPointF(-5, -5), QPointF(5, 5));
            painter.drawLine(QPointF(-5, 5), QPointF(5, -5));
        } else if (m_icon == ControlIcon::Appearance) {
            QPainterPath gear;
            for (int vertex = 0; vertex < 32; ++vertex) {
                const qreal angle = qDegreesToRadians(vertex * 11.25 - 90.0);
                const qreal radius = vertex % 4 == 1 || vertex % 4 == 2 ? 8.0 : 6.3;
                const QPointF point(qCos(angle) * radius, qSin(angle) * radius);
                if (vertex == 0)
                    gear.moveTo(point);
                else
                    gear.lineTo(point);
            }
            gear.closeSubpath();
            painter.drawPath(gear);
            painter.drawEllipse(QPointF(), 2.6, 2.6);
        } else {
            painter.drawRoundedRect(QRectF(-5, -1, 10, 8), 1.5, 1.5);
            QPainterPath shackle;
            shackle.moveTo(-3, -1);
            shackle.lineTo(-3, -4);
            shackle.cubicTo(-3, -8, 3, -8, 3, -4);
            if (m_icon == ControlIcon::Lock)
                shackle.lineTo(3, -1);
            painter.drawPath(shackle);
            painter.drawLine(QPointF(0, 2), QPointF(0, 4));
        }
        painter.restore();
        if (m_icon == ControlIcon::Unlock) {
            QFont labelFont = font();
            labelFont.setPixelSize(14);
            painter.setFont(labelFont);
            painter.setPen(QColor(242, 244, 243));
            painter.drawText(QRect(40, 0, 48, height()), Qt::AlignCenter, text());
        }
    }

    void enterEvent(QEnterEvent *event) override
    {
        QAbstractButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    ControlIcon m_icon;
};

class SubtitleControlPanel final : public QWidget
{
public:
    explicit SubtitleControlPanel(QWidget *parent = nullptr, Qt::WindowFlags flags = {})
        : QWidget(parent, flags)
    {
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
        painter.setBrush(QColor(42, 45, 44, 228));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                height() / 2.0, height() / 2.0);
    }
};

} // namespace

FloatingSubtitleWindow::FloatingSubtitleWindow()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                           | Qt::WindowDoesNotAcceptFocus)
{
    setWindowTitle(QStringLiteral("桌面字幕"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(MinimumFloatingSubtitleWidth, MinimumFloatingSubtitleHeight);
    resize(640, floatingSubtitleHeightForFont(defaultFloatingSubtitleAppearance().fontPointSize));
    setMouseTracking(true);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("floatingSubtitleScrollArea"));
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,100); min-height: 18px; border-radius: 4px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"));
    m_scrollArea->viewport()->setAutoFillBackground(false);
    m_scrollArea->viewport()->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->viewport()->setMouseTracking(true);
    m_scrollArea->viewport()->installEventFilter(this);
    m_scrollArea->verticalScrollBar()->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->verticalScrollBar()->installEventFilter(this);
    m_scrollArea->horizontalScrollBar()->setFocusPolicy(Qt::NoFocus);
    m_autoScrollTimer = new QTimer(this);
    m_autoScrollTimer->setObjectName(QStringLiteral("floatingSubtitleAutoScrollTimer"));
    m_autoScrollTimer->setInterval(30);
    m_autoScrollTimer->setTimerType(Qt::PreciseTimer);
    connect(m_autoScrollTimer, &QTimer::timeout, this, &FloatingSubtitleWindow::updateAutoScroll);
    auto *verticalBar = m_scrollArea->verticalScrollBar();
    connect(verticalBar, &QScrollBar::sliderPressed,
            this, &FloatingSubtitleWindow::takeManualScrollControl);
    connect(verticalBar, &QScrollBar::sliderMoved,
            this, &FloatingSubtitleWindow::takeManualScrollControl);
    connect(verticalBar, &QScrollBar::actionTriggered, this,
            [this](int) { takeManualScrollControl(); });
    connect(verticalBar, &QScrollBar::rangeChanged, this,
            [this](int, int) { updateAutoScroll(); });
    m_textLabel = new SubtitleTextLabel;
    m_textLabel->setObjectName(QStringLiteral("floatingSubtitleText"));
    m_textLabel->setTextFormat(Qt::PlainText);
    m_textLabel->setFocusPolicy(Qt::NoFocus);
    m_textLabel->setWordWrap(true);
    m_textLabel->setAlignment(Qt::AlignCenter);
    m_textLabel->setStyleSheet(QStringLiteral("background: transparent;"));
    // The viewport forwards body dragging while keeping its normal wheel handling.
    m_textLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_scrollArea->setWidget(m_textLabel);

    m_toolbar = new SubtitleControlPanel(this);
    m_toolbar->setObjectName(QStringLiteral("floatingSubtitleToolbar"));
    auto *toolbarLayout = new QHBoxLayout(m_toolbar);
    toolbarLayout->setContentsMargins(4, 2, 4, 2);
    toolbarLayout->setSpacing(2);
    const auto playbackButton = [this, toolbarLayout](ControlIcon icon, const QString &label,
                                                    const QString &objectName) {
        auto *button = new SubtitleControlButton(icon, label, m_toolbar);
        button->setObjectName(objectName);
        toolbarLayout->addWidget(button);
        return button;
    };
    m_previousButton = playbackButton(ControlIcon::Previous, QStringLiteral("上一首"),
                                     QStringLiteral("floatingSubtitlePreviousButton"));
    m_playPauseButton = playbackButton(ControlIcon::Play, QStringLiteral("播放"),
                                      QStringLiteral("floatingSubtitlePlayPauseButton"));
    m_nextButton = playbackButton(ControlIcon::Next, QStringLiteral("下一首"),
                                 QStringLiteral("floatingSubtitleNextButton"));
    const auto addSeparator = [this, toolbarLayout]() {
        auto *separator = new QWidget(m_toolbar);
        separator->setFixedSize(1, 18);
        separator->setStyleSheet(QStringLiteral("background: rgba(255,255,255,42);"));
        separator->setAttribute(Qt::WA_TransparentForMouseEvents);
        toolbarLayout->addSpacing(4);
        toolbarLayout->addWidget(separator);
        toolbarLayout->addSpacing(4);
    };
    addSeparator();
    m_backwardButton = playbackButton(ControlIcon::Backward, QStringLiteral("后退5秒"),
                                     QStringLiteral("floatingSubtitleBackwardButton"));
    m_forwardButton = playbackButton(ControlIcon::Forward, QStringLiteral("前进5秒"),
                                    QStringLiteral("floatingSubtitleForwardButton"));
    addSeparator();
    auto *lockButton = new SubtitleControlButton(ControlIcon::Lock, QStringLiteral("锁定"), m_toolbar);
    lockButton->setObjectName(QStringLiteral("floatingSubtitleLockButton"));
    auto *appearanceButton = new SubtitleControlButton(
        ControlIcon::Appearance, QStringLiteral("外观"), m_toolbar);
    appearanceButton->setObjectName(QStringLiteral("floatingSubtitleAppearanceButton"));
    auto *closeButton = new SubtitleControlButton(ControlIcon::Close, QStringLiteral("关闭"), m_toolbar);
    closeButton->setObjectName(QStringLiteral("floatingSubtitleCloseButton"));
    toolbarLayout->addWidget(lockButton);
    toolbarLayout->addWidget(appearanceButton);
    toolbarLayout->addWidget(closeButton);
    m_toolbar->setFixedSize(toolbarLayout->sizeHint());
    m_toolbar->hide();
    connect(m_previousButton, &QAbstractButton::clicked, this, &FloatingSubtitleWindow::previousRequested);
    connect(m_playPauseButton, &QAbstractButton::clicked, this, &FloatingSubtitleWindow::playPauseRequested);
    connect(m_nextButton, &QAbstractButton::clicked, this, &FloatingSubtitleWindow::nextRequested);
    connect(m_backwardButton, &QAbstractButton::clicked, this, [this]() { emit seekByRequested(-5000); });
    connect(m_forwardButton, &QAbstractButton::clicked, this, [this]() { emit seekByRequested(5000); });
    setPlaybackControlsState(false, false, false, false, false);
    connect(lockButton, &QAbstractButton::clicked, this, [this]() { setLocked(true); });
    connect(appearanceButton, &QAbstractButton::clicked,
            this, &FloatingSubtitleWindow::appearanceRequested);
    connect(closeButton, &QAbstractButton::clicked, this, &FloatingSubtitleWindow::hideWindow);

    m_unlockControl = std::make_unique<SubtitleControlPanel>(nullptr,
        Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
            | Qt::WindowDoesNotAcceptFocus);
    m_unlockControl->setObjectName(QStringLiteral("floatingSubtitleUnlockControl"));
    m_unlockControl->setWindowTitle(QStringLiteral("解锁桌面字幕"));
    m_unlockControl->setAttribute(Qt::WA_TranslucentBackground);
    m_unlockControl->setAttribute(Qt::WA_ShowWithoutActivating);
    m_unlockControl->setAttribute(Qt::WA_QuitOnClose, false);
    m_unlockControl->setAttribute(Qt::WA_DeleteOnClose, false);
    m_unlockControl->setFixedSize(96, 34);
    auto *unlockButton = new SubtitleControlButton(
        ControlIcon::Unlock, QStringLiteral("解锁"), m_unlockControl.get());
    unlockButton->setObjectName(QStringLiteral("floatingSubtitleUnlockButton"));
    connect(unlockButton, &QAbstractButton::clicked, this, [this]() { setLocked(false); });

    auto *layout = new QVBoxLayout(this);
    // Controls overlay this permanent strip; hover never moves the subtitle.
    layout->setContentsMargins(16, 48, 16, 12);
    layout->addWidget(m_scrollArea);
    setAppearance(defaultFloatingSubtitleAppearance());

    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setInterval(60);
    // Locked native windows receive no hover events. Global cursor coordinates
    // let their separate unlock control appear without intercepting subtitles.
    connect(m_hoverTimer, &QTimer::timeout, this, &FloatingSubtitleWindow::updateHoverControls);

    const auto topologyChanged = [this]() {
        m_screenConnectionsDirty = true;
        queueScreenLayoutCheck();
    };
    connect(qGuiApp, &QGuiApplication::screenAdded, this, topologyChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, topologyChanged);
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged,
            this, &FloatingSubtitleWindow::queueScreenLayoutCheck);
    refreshScreenConnections();
}

FloatingSubtitleWindow::~FloatingSubtitleWindow()
{
    m_hoverTimer->stop();
    m_autoScrollTimer->stop();
    m_unlockControl->hide();
}

void FloatingSubtitleWindow::setSubtitleText(const QString &text)
{
    if (m_textLabel->text() == text)
        return;
    // Text replacement must not inherit timing or manual takeover from an old cue.
    m_autoScrollEntries.clear();
    m_playing = false;
    resetAutoScroll();
    m_textLabel->setText(text);
    updateTextLayout();
    m_scrollArea->verticalScrollBar()->setValue(0);
}

void FloatingSubtitleWindow::clearText()
{
    setSubtitleText(QString());
    setSubtitlePlaybackContext({}, 0, false);
}

QString FloatingSubtitleWindow::subtitleText() const
{
    return m_textLabel->text();
}

void FloatingSubtitleWindow::showWindow()
{
    ensurePositionOnScreens();
    show();
    raise();
    updateHoverControls();
    // The show event may already have shown the independent tool. Keep it
    // above the subtitle after raising the subtitle's own native window.
    if (m_unlockControl->isVisible())
        m_unlockControl->raise();
}

void FloatingSubtitleWindow::hideWindow()
{
    hide();
}

void FloatingSubtitleWindow::setLocked(bool locked)
{
    if (m_locked == locked)
        return;

    m_locked = locked;
    m_dragging = false;
    m_resizeEdges = {};
    unsetCursor();
    if (QWidget::mouseGrabber() == this)
        releaseMouse();

    const QRect previousGeometry = geometry();
    const bool wasVisible = isVisible();
    {
        // Changing native window flags may hide/recreate the window. These
        // intermediate events must not change the caller's visibility control.
        const QScopedValueRollback<bool> changingFlags(m_changingWindowFlags, true);
        const QSignalBlocker blocker(this);
        auto flags = windowFlags();
        flags.setFlag(Qt::WindowTransparentForInput, m_locked);
        setWindowFlags(flags);
        setGeometry(previousGeometry);
        if (wasVisible) {
            showWindow();
            setGeometry(previousGeometry);
        }
    }
    emit lockedChanged(m_locked);
    updateHoverControls();
    queueScreenLayoutCheck();
}

bool FloatingSubtitleWindow::isLocked() const
{
    return m_locked;
}

void FloatingSubtitleWindow::setPlaybackControlsState(bool playing, bool canToggle, bool canSeek,
                                                     bool canPrevious, bool canNext)
{
    static_cast<SubtitleControlButton *>(m_playPauseButton)->setIcon(
        playing ? ControlIcon::Pause : ControlIcon::Play);
    const QString label = playing ? QStringLiteral("暂停") : QStringLiteral("播放");
    m_playPauseButton->setAccessibleName(label);
    m_playPauseButton->setToolTip(label);
    m_playPauseButton->setProperty("playing", playing);
    m_playPauseButton->setEnabled(canToggle);
    m_previousButton->setEnabled(canPrevious);
    m_nextButton->setEnabled(canNext);
    m_backwardButton->setEnabled(canSeek);
    m_forwardButton->setEnabled(canSeek);
}

void FloatingSubtitleWindow::setSubtitlePlaybackContext(const QList<SubtitleEntry> &entries,
                                                       qint64 positionMs, bool playing)
{
    const auto sortedEntries = sortedSubtitleEntries(entries);
    const bool cueChanged = !sameSubtitleEntries(m_autoScrollEntries, sortedEntries);
    if (cueChanged) {
        m_autoScrollEntries = sortedEntries;
        resetAutoScroll();
    }
    positionMs = qMax(qint64(0), positionMs);
    if (!cueChanged && positionMs < m_playbackPositionMs
        && m_playbackPositionMs - positionMs >= 250) {
        // A backend loop or other substantial backwards discontinuity may not
        // pass through the UI seek action. Small timestamp corrections keep a
        // paused reader's manual takeover intact.
        resetAutoScroll();
    }
    const bool advanced = positionMs != m_playbackPositionMs;
    const bool resumed = playing && !m_playing;
    m_playbackPositionMs = positionMs;
    m_playing = playing;
    if (m_autoScrollNeedsAnchor && !m_autoScrollEntries.isEmpty()) {
        m_autoScrollNeedsAnchor = false;
        m_autoScrollAnchorMs = positionMs;
        m_autoScrollEndMs = m_autoScrollEntries.first().endMs;
        for (const auto &entry : m_autoScrollEntries)
            m_autoScrollEndMs = qMin(m_autoScrollEndMs, entry.endMs);
        m_playbackSnapshotClock.restart();
    } else if (advanced || resumed) {
        // Repeated snapshots at the same media position do not extend a stalled
        // player's clock. Pauses contribute no wall time to the scroll position.
        m_playbackSnapshotClock.restart();
    }
    if (!m_playing)
        m_playbackSnapshotClock.invalidate();
    updateAutoScroll();
}

void FloatingSubtitleWindow::resetAutoScroll()
{
    m_autoScrollTimer->stop();
    m_autoScrollNeedsAnchor = true;
    m_manualScroll = false;
    m_playbackSnapshotClock.invalidate();
    m_scrollArea->verticalScrollBar()->setValue(0);
}

void FloatingSubtitleWindow::takeManualScrollControl()
{
    if (m_locked || m_autoScrollEntries.isEmpty())
        return;
    m_manualScroll = true;
    m_autoScrollTimer->stop();
}

void FloatingSubtitleWindow::updateAutoScroll()
{
    if (m_reflowing)
        return;
    auto *bar = m_scrollArea->verticalScrollBar();
    if (!isVisible() || !m_playing || m_manualScroll || m_autoScrollNeedsAnchor
        || m_autoScrollEntries.isEmpty() || bar->maximum() <= 0) {
        m_autoScrollTimer->stop();
        return;
    }
    constexpr qint64 HeadHoldMs = 1000;
    constexpr qint64 TailHoldMs = 1000;
    constexpr qreal MinimumPixelsPerSecond = 12.0;
    constexpr qreal MaximumPixelsPerSecond = 30.0;
    constexpr qint64 MaximumExtrapolationMs = 250;
    qint64 positionMs = m_playbackPositionMs;
    const qint64 extrapolationMs = m_playbackSnapshotClock.isValid()
        ? m_playbackSnapshotClock.elapsed() : 0;
    positionMs = qMin(positionMs, m_autoScrollEndMs);
    // Bound before adding: valid subtitle timestamps can approach qint64 max.
    positionMs += qMin(qMin(extrapolationMs, MaximumExtrapolationMs),
                       m_autoScrollEndMs - positionMs);
    const qint64 elapsedMs = qMax(qint64(0), positionMs - m_autoScrollAnchorMs - HeadHoldMs);
    const qint64 availableMs = m_autoScrollEndMs - m_autoScrollAnchorMs
        - HeadHoldMs - TailHoldMs;
    // Very long cue intervals must still move at a readable speed rather than
    // waiting indefinitely for their scheduled end.
    const qreal speed = availableMs > 0
        ? qBound(MinimumPixelsPerSecond, bar->maximum() * 1000.0 / availableMs,
                 MaximumPixelsPerSecond)
        : MaximumPixelsPerSecond;
    const bool reachedTailHold = availableMs > 0 && elapsedMs >= availableMs
        && bar->maximum() * 1000.0 <= MaximumPixelsPerSecond * availableMs;
    const qreal distance = reachedTailHold ? bar->maximum()
        : qMin(qreal(bar->maximum()), elapsedMs * speed / 1000.0);
    // Position notifications may trail the last bounded interpolation slightly.
    // Do not make that correction visibly pull an already read line backwards.
    bar->setValue(qMax(bar->value(), int(std::floor(distance))));
    if (bar->value() >= bar->maximum() || positionMs >= m_autoScrollEndMs
        || extrapolationMs >= MaximumExtrapolationMs) {
        // New media snapshots restart interpolation; a stalled source leaves
        // neither an advancing subtitle nor a continuously running timer.
        m_autoScrollTimer->stop();
    } else if (!m_autoScrollTimer->isActive()) {
        m_autoScrollTimer->start();
    }
}

void FloatingSubtitleWindow::setAppearance(const FloatingSubtitleAppearance &appearance)
{
    auto validated = validatedFloatingSubtitleAppearance(appearance);
    const int heightLimit = m_resizeEdges ? m_resizeMaximumHeight : availableMaximumHeight();
    while (validated.fontPointSize > MinimumFloatingFontPointSize
           && floatingSubtitleHeightForFont(validated.fontPointSize) > heightLimit)
        --validated.fontPointSize;
    const bool changed = m_appearance != validated;
    m_appearance = validated;
    const QPoint previousPosition = pos();
    QFont textFont = m_textLabel->font();
    textFont.setFamily(m_appearance.fontFamily);
    textFont.setPointSize(m_appearance.fontPointSize);
    textFont.setBold(true);
    m_textLabel->setFont(textFont);
    QPalette textPalette = m_textLabel->palette();
    textPalette.setColor(QPalette::WindowText, m_appearance.fontColor);
    m_textLabel->setPalette(textPalette);
    setMaximumHeight(heightLimit);
    resize(width(), floatingSubtitleHeightForFont(m_appearance.fontPointSize));
    updateTextLayout();
    move(previousPosition);
    update();
    if (changed)
        emit appearanceChanged(m_appearance);
}

FloatingSubtitleAppearance FloatingSubtitleWindow::appearance() const
{
    return m_appearance;
}

void FloatingSubtitleWindow::restorePosition(std::optional<QPoint> savedPosition)
{
    if (m_dragging || m_resizeEdges || m_changingWindowFlags || m_correctingPosition)
        return;
    QList<QRect> availableScreens;
    for (const auto *screen : QGuiApplication::screens()) {
        const QRect area = screen->availableGeometry();
        if (!area.isEmpty())
            availableScreens.append(area);
    }
    // Display reconfiguration can briefly leave no usable screen. Keep the last
    // position until a later screenAdded/geometry notification supplies one.
    if (availableScreens.isEmpty())
        return;
    const auto *primary = QGuiApplication::primaryScreen();
    const QPoint restored = safeFloatingSubtitlePosition(savedPosition, size(),
        availableScreens, primary ? primary->availableGeometry() : QRect());
    if (restored != pos()) {
        const QScopedValueRollback<bool> correcting(m_correctingPosition, true);
        move(restored);
    }
}

void FloatingSubtitleWindow::restoreSize(std::optional<QSize> savedSize)
{
    if (savedSize && savedSize->isValid())
        resize(qMax(MinimumFloatingSubtitleWidth, savedSize->width()), height());
    setAppearance(m_appearance);
    ensurePositionOnScreens();
}

int FloatingSubtitleWindow::availableMaximumHeight() const
{
    const auto *currentScreen = QGuiApplication::screenAt(frameGeometry().center());
    if (!currentScreen)
        currentScreen = screen();
    if (!currentScreen)
        currentScreen = QGuiApplication::primaryScreen();
    return currentScreen
        ? qBound(MinimumFloatingSubtitleHeight, currentScreen->availableGeometry().height(),
                 MaximumFloatingSubtitleHeight)
        : MaximumFloatingSubtitleHeight;
}

void FloatingSubtitleWindow::ensurePositionOnScreens()
{
    if (!m_dragging && !m_resizeEdges && !m_changingWindowFlags)
        setAppearance(m_appearance);
    restorePosition(pos());
}

void FloatingSubtitleWindow::updateTextLayout()
{
    if (m_reflowing)
        return;
    auto *bar = m_scrollArea->verticalScrollBar();
    const int oldMaximum = bar->maximum();
    const int oldValue = bar->value();
    {
        // Reflow is neither a new cue nor manual scrolling. Retain the reader's
        // relative position when wrapping changes, including a manual takeover.
        const QScopedValueRollback<bool> reflowing(m_reflowing, true);
        const QSize viewport = m_scrollArea->viewport()->size();
        const int textWidth = qMax(1, viewport.width());
        const int textHeight = qMax(viewport.height(), m_textLabel->heightForWidth(textWidth));
        m_textLabel->resize(textWidth, textHeight);
        if (oldMaximum > 0)
            bar->setValue(qRound(qreal(oldValue) * bar->maximum() / oldMaximum));
    }
    updateAutoScroll();
}

void FloatingSubtitleWindow::updateControlPositions()
{
    if (!m_toolbar || !m_unlockControl)
        return;
    m_toolbar->move((width() - m_toolbar->width()) / 2, 6);
    QPoint unlockPosition = mapToGlobal(QPoint((width() - m_unlockControl->width()) / 2, 7));
    // A recoverable subtitle may be partly offscreen. Keep its independent
    // unlock pill on the screen showing the largest portion of the subtitle.
    const QRect windowArea(mapToGlobal(QPoint()), size());
    QRect bestArea;
    qint64 bestIntersection = 0;
    for (const auto *screen : QGuiApplication::screens()) {
        const QRect available = screen->availableGeometry();
        const QRect intersection = windowArea.intersected(available);
        const qint64 area = qint64(intersection.width()) * intersection.height();
        if (area > bestIntersection) {
            bestArea = available;
            bestIntersection = area;
        }
    }
    if (!bestArea.isEmpty()) {
        unlockPosition.setX(qBound(bestArea.left(), unlockPosition.x(),
            qMax(bestArea.left(), bestArea.right() + 1 - m_unlockControl->width())));
        unlockPosition.setY(qBound(bestArea.top(), unlockPosition.y(),
            qMax(bestArea.top(), bestArea.bottom() + 1 - m_unlockControl->height())));
    }
    m_unlockControl->move(unlockPosition);
}

void FloatingSubtitleWindow::updateHoverControls()
{
    if (!m_toolbar || !m_unlockControl)
        return;
    if (!isVisible() || m_changingWindowFlags) {
        m_toolbar->hide();
        m_unlockControl->hide();
        return;
    }
    updateControlPositions();
    const QPoint cursor = QCursor::pos();
    const bool hovering = QRect(mapToGlobal(QPoint()), size()).contains(cursor)
        || (m_unlockControl->isVisible() && m_unlockControl->geometry().contains(cursor));
    m_toolbar->setVisible(!m_locked && hovering);
    if (m_locked && hovering) {
        if (!m_unlockControl->isVisible()) {
            m_unlockControl->show();
            m_unlockControl->raise();
        }
    } else {
        m_unlockControl->hide();
    }
}

void FloatingSubtitleWindow::refreshScreenConnections()
{
    for (const auto &connection : m_screenConnections)
        disconnect(connection);
    m_screenConnections.clear();
    // Store connection handles only. A queued callback never captures a QScreen
    // pointer: removed screens can be deleted before that callback executes.
    for (auto *screen : QGuiApplication::screens()) {
        m_screenConnections.append(connect(screen, &QScreen::availableGeometryChanged,
            this, &FloatingSubtitleWindow::queueScreenLayoutCheck));
        m_screenConnections.append(connect(screen, &QScreen::geometryChanged,
            this, &FloatingSubtitleWindow::queueScreenLayoutCheck));
        m_screenConnections.append(connect(screen, &QScreen::logicalDotsPerInchChanged,
            this, &FloatingSubtitleWindow::queueScreenLayoutCheck));
        m_screenConnections.append(connect(screen, &QObject::destroyed, this, [this]() {
            m_screenConnectionsDirty = true;
            queueScreenLayoutCheck();
        }));
    }
    m_screenConnectionsDirty = false;
}

void FloatingSubtitleWindow::observeWindowScreen()
{
    QWindow *nativeWindow = windowHandle();
    if (m_observedWindow == nativeWindow)
        return;
    disconnect(m_windowScreenConnection);
    m_observedWindow = nativeWindow;
    if (nativeWindow) {
        m_windowScreenConnection = connect(nativeWindow, &QWindow::screenChanged,
            this, &FloatingSubtitleWindow::queueScreenLayoutCheck);
    }
}

void FloatingSubtitleWindow::queueScreenLayoutCheck()
{
    if (m_screenLayoutCheckQueued)
        return;
    m_screenLayoutCheckQueued = true;
    // Coalesce bursts and let Qt finish its own screen removal / DPI transition.
    // The receiver context cancels this functor when the window is destroyed.
    QMetaObject::invokeMethod(this, [this]() {
        m_screenLayoutCheckQueued = false;
        if (m_screenConnectionsDirty)
            refreshScreenConnections();
        ensurePositionOnScreens();
    }, Qt::QueuedConnection);
}

bool FloatingSubtitleWindow::event(QEvent *event)
{
    const auto type = event->type();
    const bool handled = QWidget::event(event);
    if (type == QEvent::WinIdChange)
        observeWindowScreen();
    const bool stableMove = type == QEvent::Move
        && !m_dragging && !m_resizeEdges && !m_changingWindowFlags && !m_correctingPosition;
    if (type == QEvent::DevicePixelRatioChange || type == QEvent::Resize || stableMove) {
        // Qt scales painting/fonts in device pixels. Only logical placement is
        // checked here; a smaller work area also constrains the font-linked height.
        // A late native move can overwrite an earlier hidden-widget correction;
        // stable Move notifications therefore also recheck the final position.
        queueScreenLayoutCheck();
    } else if (type == QEvent::UngrabMouse && (m_dragging || m_resizeEdges)) {
        m_dragging = false;
        m_resizeEdges = {};
        queueScreenLayoutCheck();
    }
    if (type == QEvent::Resize && m_textLabel)
        updateTextLayout();
    if (type == QEvent::Leave && !m_dragging && !m_resizeEdges)
        unsetCursor();
    if (type == QEvent::Move || type == QEvent::Resize
        || type == QEvent::DevicePixelRatioChange || type == QEvent::Enter
        || type == QEvent::Leave) {
        updateHoverControls();
    }
    return handled;
}

bool FloatingSubtitleWindow::eventFilter(QObject *watched, QEvent *event)
{
    const bool scrollBar = watched == m_scrollArea->verticalScrollBar();
    const bool viewport = watched == m_scrollArea->viewport();
    if ((scrollBar || viewport) && event->type() == QEvent::Wheel)
        takeManualScrollControl();
    if (scrollBar && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
        takeManualScrollControl();
    if (!viewport)
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::Resize && !m_textLayoutUpdateQueued) {
        m_textLayoutUpdateQueued = true;
        // A newly visible scrollbar changes the wrapping width. Recalculate
        // after the scroll area has applied that viewport resize.
        QMetaObject::invokeMethod(this, [this]() {
            m_textLayoutUpdateQueued = false;
            updateTextLayout();
        }, Qt::QueuedConnection);
    } else if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            mousePressEvent(mouse);
            return !m_locked;
        }
    } else if (event->type() == QEvent::MouseMove) {
        mouseMoveEvent(static_cast<QMouseEvent *>(event));
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && (m_dragging || m_resizeEdges)) {
            mouseReleaseEvent(mouse);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FloatingSubtitleWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const int backgroundAlpha = qRound(m_appearance.backgroundOpacityPercent * 255.0 / 100.0);
    painter.setBrush(QColor(20, 20, 20, backgroundAlpha));
    painter.drawRoundedRect(rect(), 10, 10);
}

Qt::Edges FloatingSubtitleWindow::resizeEdgesAt(QPoint localPosition) const
{
    if (m_locked || !rect().contains(localPosition))
        return {};
    constexpr int EdgeWidth = 8;
    Qt::Edges edges;
    if (localPosition.x() < EdgeWidth)
        edges |= Qt::LeftEdge;
    else if (localPosition.x() >= width() - EdgeWidth)
        edges |= Qt::RightEdge;
    if (localPosition.y() < EdgeWidth)
        edges |= Qt::TopEdge;
    else if (localPosition.y() >= height() - EdgeWidth)
        edges |= Qt::BottomEdge;
    return edges;
}

void FloatingSubtitleWindow::updateResizeCursor(QPoint localPosition)
{
    const auto edges = m_resizeEdges ? m_resizeEdges : resizeEdgesAt(localPosition);
    if ((edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge))
        || (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge)))
        setCursor(Qt::SizeFDiagCursor);
    else if ((edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::TopEdge))
             || (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::BottomEdge)))
        setCursor(Qt::SizeBDiagCursor);
    else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge))
        setCursor(Qt::SizeHorCursor);
    else if (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge))
        setCursor(Qt::SizeVerCursor);
    else
        unsetCursor();
}

void FloatingSubtitleWindow::applyResize(QPoint globalPosition)
{
    const QPoint delta = globalPosition - m_resizeStartPosition;
    QRect next = m_resizeStartGeometry;
    if (m_resizeEdges.testFlag(Qt::LeftEdge))
        next.setLeft(qMin(next.right() - MinimumFloatingSubtitleWidth + 1, next.left() + delta.x()));
    else if (m_resizeEdges.testFlag(Qt::RightEdge))
        next.setWidth(qMax(MinimumFloatingSubtitleWidth, next.width() + delta.x()));
    if (m_resizeEdges.testFlag(Qt::TopEdge) || m_resizeEdges.testFlag(Qt::BottomEdge)) {
        const int requestedHeight = qBound(MinimumFloatingSubtitleHeight,
            m_resizeStartGeometry.height()
                + (m_resizeEdges.testFlag(Qt::TopEdge) ? -delta.y() : delta.y()),
            m_resizeMaximumHeight);
        auto resizedAppearance = m_appearance;
        resizedAppearance.fontPointSize = floatingSubtitleFontForHeight(requestedHeight);
        setAppearance(resizedAppearance);
        const int targetHeight = floatingSubtitleHeightForFont(m_appearance.fontPointSize);
        if (m_resizeEdges.testFlag(Qt::TopEdge))
            next.setTop(m_resizeStartGeometry.bottom() - targetHeight + 1);
        else
            next.setHeight(targetHeight);
    }
    setGeometry(next);
}

void FloatingSubtitleWindow::mousePressEvent(QMouseEvent *event)
{
    if (m_locked) {
        m_dragging = false;
        m_resizeEdges = {};
        event->ignore();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_resizeEdges = resizeEdgesAt(mapFromGlobal(event->globalPosition().toPoint()));
        m_dragging = !m_resizeEdges;
        m_dragOffset = event->globalPosition() - QPointF(frameGeometry().topLeft());
        if (m_resizeEdges) {
            m_resizeStartGeometry = geometry();
            m_resizeStartPosition = event->globalPosition().toPoint();
            m_resizeMaximumHeight = availableMaximumHeight();
            updateResizeCursor(mapFromGlobal(m_resizeStartPosition));
            grabMouse();
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void FloatingSubtitleWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_locked) {
        m_dragging = false;
        m_resizeEdges = {};
        event->ignore();
        return;
    }
    if (event->buttons().testFlag(Qt::LeftButton)) {
        if (m_resizeEdges) {
            applyResize(event->globalPosition().toPoint());
            event->accept();
            return;
        }
        if (m_dragging) {
            move((event->globalPosition() - m_dragOffset).toPoint());
            event->accept();
            return;
        }
    }
    m_dragging = false;
    m_resizeEdges = {};
    updateResizeCursor(mapFromGlobal(event->globalPosition().toPoint()));
    QWidget::mouseMoveEvent(event);
}

void FloatingSubtitleWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_locked) {
        m_dragging = false;
        m_resizeEdges = {};
        event->ignore();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (m_resizeEdges)
            applyResize(event->globalPosition().toPoint());
        m_dragging = false;
        m_resizeEdges = {};
        if (QWidget::mouseGrabber() == this)
            releaseMouse();
        updateResizeCursor(mapFromGlobal(event->globalPosition().toPoint()));
        queueScreenLayoutCheck();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void FloatingSubtitleWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    observeWindowScreen();
    queueScreenLayoutCheck();
    m_hoverTimer->start();
    updateAutoScroll();
    updateHoverControls();
    emit visibilityChanged(true);
}

void FloatingSubtitleWindow::hideEvent(QHideEvent *event)
{
    m_dragging = false;
    m_resizeEdges = {};
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
    m_hoverTimer->stop();
    m_autoScrollTimer->stop();
    m_toolbar->hide();
    m_unlockControl->hide();
    QWidget::hideEvent(event);
    queueScreenLayoutCheck();
    emit visibilityChanged(false);
}

void FloatingSubtitleWindow::closeEvent(QCloseEvent *event)
{
    m_dragging = false;
    m_resizeEdges = {};
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
    m_hoverTimer->stop();
    m_autoScrollTimer->stop();
    m_toolbar->hide();
    m_unlockControl->hide();
    // QWidget's accepted close path hides the window. Both close-related
    // attributes stay false, so the instance survives and never quits the app.
    event->accept();
}

} // namespace asmr
