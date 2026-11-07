#include "ui/PlaybackControlsWidget.h"

#include "ui/CoverArtWidget.h"
#include "ui/TimeFormat.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPaintEvent>
#include <QMoveEvent>
#include <QShowEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QWidgetAction>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace asmr {
namespace {

// Local painted icons avoid font-dependent glyphs and another asset dependency.
// These are presentation only; clicks still travel through the existing intentions.
enum class BarIcon { Play, Pause, Previous, Next, Queue, Volume, Muted, Sequential, RepeatAll, RepeatOne, Shuffle, Subtitles, Panel, Desktop };

BarIcon modeIcon(PlaybackMode mode)
{
    switch (mode) {
    case PlaybackMode::RepeatAll: return BarIcon::RepeatAll;
    case PlaybackMode::RepeatOne: return BarIcon::RepeatOne;
    case PlaybackMode::Shuffle: return BarIcon::Shuffle;
    default: return BarIcon::Sequential;
    }
}

QString modeName(PlaybackMode mode)
{
    switch (mode) {
    case PlaybackMode::RepeatAll: return QStringLiteral("列表循环");
    case PlaybackMode::RepeatOne: return QStringLiteral("单曲循环");
    case PlaybackMode::Shuffle: return QStringLiteral("随机播放");
    default: return QStringLiteral("顺序播放");
    }
}

QIcon barIcon(BarIcon symbol, const QColor &normal = QColor("#BDC4CE"))
{
    QIcon icon;
    const bool popupControl = symbol == BarIcon::Sequential || symbol == BarIcon::RepeatAll
        || symbol == BarIcon::RepeatOne || symbol == BarIcon::Shuffle
        || symbol == BarIcon::Volume || symbol == BarIcon::Muted;
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected}) {
        const QColor color = mode == QIcon::Disabled ? QColor("#69717D")
            : mode == QIcon::Active && popupControl && normal == QColor("#BDC4CE")
                ? QColor("#F4F4F4") : normal;
        for (const qreal scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
            QPixmap pixmap(qRound(24 * scale), qRound(24 * scale));
            pixmap.setDevicePixelRatio(scale);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            switch (symbol) {
            case BarIcon::Play: {
                QPainterPath triangle;
                triangle.moveTo(8, 5);
                triangle.lineTo(19, 12);
                triangle.lineTo(8, 19);
                triangle.closeSubpath();
                painter.fillPath(triangle, color);
                break;
            }
            case BarIcon::Pause:
                painter.fillRect(QRectF(6.5, 5, 4, 14), color);
                painter.fillRect(QRectF(13.5, 5, 4, 14), color);
                break;
            case BarIcon::Sequential:
            case BarIcon::RepeatAll:
            case BarIcon::RepeatOne: {
                // Shared rounded loop. Arrowheads sit on the path rather than
                // crossing its corners, keeping all three modes optically alike.
                painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                QPainterPath loop;
                loop.moveTo(14.5, 5);
                loop.lineTo(7, 5);
                loop.cubicTo(4.5, 5, 3, 6.5, 3, 9);
                loop.lineTo(3, 15);
                loop.cubicTo(3, 17.5, 4.5, 19, 7, 19);
                if (symbol == BarIcon::Sequential) {
                    loop.lineTo(17, 19);
                    loop.cubicTo(19.5, 19, 21, 17.5, 21, 15);
                    loop.lineTo(21, 9);
                    loop.cubicTo(21, 6.5, 19.5, 5, 17, 5);
                    loop.lineTo(14.5, 5);
                } else {
                    loop.lineTo(9.5, 19);
                    loop.moveTo(9.5, 19);
                    loop.lineTo(17, 19);
                    loop.cubicTo(19.5, 19, 21, 17.5, 21, 15);
                    loop.lineTo(21, 9);
                    loop.cubicTo(21, 6.5, 19.5, 5, 17, 5);
                    loop.lineTo(14.5, 5);
                }
                painter.drawPath(loop);
                painter.drawLine(QPointF(12, 2.5), QPointF(14.5, 5));
                painter.drawLine(QPointF(14.5, 5), QPointF(12, 7.5));
                if (symbol != BarIcon::Sequential) {
                    painter.drawLine(QPointF(12, 16.5), QPointF(9.5, 19));
                    painter.drawLine(QPointF(9.5, 19), QPointF(12, 21.5));
                }
                if (symbol == BarIcon::RepeatOne) {
                    painter.drawLine(QPointF(10.5, 10), QPointF(12.2, 8.8));
                    painter.drawLine(QPointF(12.2, 8.8), QPointF(12.2, 15.2));
                    painter.drawLine(QPointF(10.5, 15.2), QPointF(13.8, 15.2));
                }
                break;
            }
            case BarIcon::Shuffle: {
                painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                QPainterPath upper;
                upper.moveTo(3, 6);
                upper.lineTo(5.5, 6);
                upper.cubicTo(10, 6, 14, 18, 18.5, 18);
                upper.lineTo(21, 18);
                painter.drawPath(upper);
                QPainterPath lower;
                lower.moveTo(3, 18);
                lower.lineTo(5.5, 18);
                lower.cubicTo(10, 18, 14, 6, 18.5, 6);
                lower.lineTo(21, 6);
                painter.drawPath(lower);
                painter.drawLine(QPointF(18.5, 3.5), QPointF(21, 6));
                painter.drawLine(QPointF(18.5, 8.5), QPointF(21, 6));
                painter.drawLine(QPointF(18.5, 15.5), QPointF(21, 18));
                painter.drawLine(QPointF(18.5, 20.5), QPointF(21, 18));
                break;
            }
            case BarIcon::Previous:
            case BarIcon::Next: {
                if (symbol == BarIcon::Previous) {
                    painter.translate(24, 0);
                    painter.scale(-1, 1);
                }
                QPainterPath triangle;
                triangle.moveTo(6, 5);
                triangle.lineTo(16, 12);
                triangle.lineTo(6, 19);
                triangle.closeSubpath();
                painter.fillPath(triangle, color);
                painter.drawLine(QPointF(18, 5), QPointF(18, 19));
                break;
            }
            case BarIcon::Queue:
                painter.drawLine(QPointF(4, 6), QPointF(20, 6));
                painter.drawLine(QPointF(4, 12), QPointF(20, 12));
                painter.drawLine(QPointF(4, 18), QPointF(14, 18));
                break;
            case BarIcon::Volume:
            case BarIcon::Muted: {
                painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                QPainterPath speaker;
                speaker.moveTo(4, 9);
                speaker.lineTo(8, 9);
                speaker.lineTo(13, 5);
                speaker.lineTo(13, 19);
                speaker.lineTo(8, 15);
                speaker.lineTo(4, 15);
                speaker.closeSubpath();
                painter.drawPath(speaker);
                if (symbol == BarIcon::Muted) {
                    painter.drawLine(QPointF(17, 8), QPointF(21, 16));
                } else {
                    QPainterPath wave;
                    wave.moveTo(17, 7);
                    wave.cubicTo(21, 9.5, 21, 14.5, 17, 17);
                    painter.drawPath(wave);
                }
                break;
            }
            case BarIcon::Subtitles:
                painter.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
                painter.drawLine(QPointF(6, 10), QPointF(11, 10));
                painter.drawLine(QPointF(14, 10), QPointF(18, 10));
                painter.drawLine(QPointF(6, 14), QPointF(9, 14));
                painter.drawLine(QPointF(12, 14), QPointF(18, 14));
                break;
            case BarIcon::Panel:
                painter.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
                painter.drawLine(QPointF(15, 4), QPointF(15, 20));
                painter.drawLine(QPointF(6, 9), QPointF(12, 9));
                painter.drawLine(QPointF(6, 13), QPointF(11, 13));
                break;
            case BarIcon::Desktop:
                painter.drawRoundedRect(QRectF(2.5, 3.5, 19, 14), 2, 2);
                painter.drawLine(QPointF(8, 21), QPointF(16, 21));
                painter.drawLine(QPointF(12, 18), QPointF(12, 21));
                painter.drawLine(QPointF(6, 12), QPointF(18, 12));
                painter.drawLine(QPointF(8, 8), QPointF(16, 8));
                break;
            }
            painter.end();
            icon.addPixmap(pixmap, mode);
        }
    }
    return icon;
}

class PlaybackPopupMenu final : public QMenu
{
public:
    explicit PlaybackPopupMenu(QWidget *parent, bool modeMenu)
        : QMenu(parent), m_modeMenu(modeMenu)
    {
        setWindowFlag(Qt::FramelessWindowHint);
        setWindowFlag(Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoMouseReplay);
        // Reserve both possible tail strips. Flipping never changes QMenu's
        // action geometry, and neither strip can activate an adjacent action.
        setContentsMargins(0, tailHeight, 0, tailHeight);
    }

    void popupAt(const QRect &anchor, const QRect &available)
    {
        m_anchor = anchor;
        ensurePolished();
        // The Windows 11 Qt style installs a separate graphics-effect shadow
        // during polish, independently of NoDropShadowWindowHint. This call is
        // in the popup-opening input path, before any effect paint can begin.
        removeStyleShadow();
        adjustSize();
        const QSize popupSize = sizeHint().boundedTo(available.size());
        resize(popupSize);
        m_above = anchor.top() - popupSize.height() - anchorGap >= available.top();
        int x = anchor.center().x() - popupSize.width() / 2;
        int y = m_above ? anchor.top() - popupSize.height() - anchorGap
                        : anchor.bottom() + anchorGap;
        x = std::clamp(x, available.left(), available.right() - popupSize.width() + 1);
        y = std::clamp(y, available.top(), available.bottom() - popupSize.height() + 1);
        popup(QPoint(x, y));
        // QMenu or the native platform can adjust the requested position.
        // Keep the actual window inside the work area and derive the pointer
        // from that final geometry rather than the originally requested x.
        move(std::clamp(pos().x(), available.left(), available.right() - width() + 1),
             std::clamp(pos().y(), available.top(), available.bottom() - height() + 1));
        updateAttachment();
    }

protected:
    bool event(QEvent *event) override
    {
        const bool handled = QMenu::event(event);
        if (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange) {
            // A style may repolish an already visible menu. Defer effect
            // ownership changes until the event returns, outside any paint.
            if (!m_shadowRemovalQueued) {
                m_shadowRemovalQueued = true;
                QTimer::singleShot(0, this, [this] {
                    m_shadowRemovalQueued = false;
                    removeStyleShadow();
                });
            }
        }
        return handled;
    }

    void paintEvent(QPaintEvent *event) override
    {
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#282828")));
            painter.drawPath(silhouette());
        }
        if (!m_modeMenu) {
            QMenu::paintEvent(event);
            return;
        }
        // Paint the existing QAction rows at a consistent 24 logical-pixel
        // icon size. Qt still supplies hit testing, selection and shortcuts.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(font());
        for (auto *action : actions()) {
            const QRect row = actionGeometry(action);
            if (row.isEmpty() || !event->rect().intersects(row)) continue;
            if (action->isSeparator()) {
                painter.setPen(QColor(QStringLiteral("#414141")));
                painter.drawLine(row.left() + 4, row.center().y(),
                                 row.right() - 4, row.center().y());
                continue;
            }
            const bool current = action->isChecked();
            const bool highlighted = action == activeAction();
            const QColor foreground(action->isEnabled()
                ? (current ? "#1ED760" : "#F4F4F4") : "#777777");
            if (current || highlighted) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(highlighted ? "#393939" : "#2D3530"));
                painter.drawRoundedRect(row.adjusted(1, 1, -1, -1), 5, 5);
            }
            const int textWidth = fontMetrics().horizontalAdvance(action->text());
            constexpr int iconWidth = 24;
            constexpr int iconTextGap = 12;
            const int groupWidth = iconWidth + iconTextGap + textWidth;
            const int iconX = row.left() + (row.width() - groupWidth) / 2;
            const int iconY = row.center().y() - 12;
            const auto mode = static_cast<PlaybackMode>(action->data().toInt());
            const quint64 iconKey = (static_cast<quint64>(mode) << 32) | foreground.rgba();
            auto &icon = m_rowIcons[iconKey];
            if (icon.isNull()) icon = barIcon(modeIcon(mode), foreground);
            icon.paint(&painter, QRect(iconX, iconY, 24, 24));
            painter.setPen(foreground);
            painter.drawText(QRect(iconX + iconWidth + iconTextGap, row.top(), textWidth, row.height()),
                             Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, action->text());
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QMenu::resizeEvent(event);
        updateAttachment();
    }

    void moveEvent(QMoveEvent *event) override
    {
        QMenu::moveEvent(event);
        updateAttachment();
    }

    void showEvent(QShowEvent *event) override
    {
        QMenu::showEvent(event);
        updateAttachment();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (rect().contains(event->position().toPoint())) {
            if (!silhouette().contains(event->position())) {
                hide(); // Transparent corners are outside the visible popup.
                event->accept();
                return;
            }
            if (!bodyRect().contains(event->position().toPoint())) {
                event->accept(); // The pointer is decoration, never an action.
                return;
            }
        }
        QMenu::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (rect().contains(event->position().toPoint())
            && !bodyRect().contains(event->position().toPoint())) {
            event->accept();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }

private:
    void removeStyleShadow()
    {
        if (qobject_cast<QGraphicsDropShadowEffect *>(graphicsEffect()))
            setGraphicsEffect(nullptr); // QWidget deletes the owned old effect.
    }

    [[nodiscard]] QRect bodyRect() const
    {
        return QRect(0, tailHeight, width(), std::max(0, height() - 2 * tailHeight));
    }

    [[nodiscard]] QPoint tailTip() const
    {
        const int inset = cornerRadius + tailHalfWidth + 1;
        const int x = width() > 2 * inset
            ? std::clamp(m_anchor.center().x() - pos().x(), inset, width() - inset)
            : width() / 2;
        return QPoint(x, m_above ? height() - 1 : 0);
    }

    [[nodiscard]] QPainterPath silhouette() const
    {
        const QRect body = bodyRect();
        const QPoint tip = tailTip();
        QPainterPath bodyPath;
        bodyPath.addRoundedRect(QRectF(body), cornerRadius, cornerRadius);
        const qreal baseY = m_above ? body.bottom() : body.top() + 1;
        QPainterPath tailPath;
        tailPath.moveTo(tip.x() - tailHalfWidth, baseY);
        tailPath.lineTo(tip);
        tailPath.lineTo(tip.x() + tailHalfWidth, baseY);
        tailPath.closeSubpath();
        // A real union avoids opposite winding cancelling the overlap at
        // the seam, which otherwise exposes a transparent hairline at DPI.
        return bodyPath.united(tailPath);
    }

    void updateAttachment()
    {
        if (!m_anchor.isEmpty())
            m_above = geometry().center().y() < m_anchor.center().y();
        setProperty("playbackPopupBodyRect", bodyRect());
        setProperty("playbackPopupTailTip", tailTip());
        setProperty("playbackPopupAbove", m_above);
        update();
    }

    static constexpr int tailHeight = 10;
    static constexpr int tailHalfWidth = 7;
    static constexpr int cornerRadius = 8;
    static constexpr int anchorGap = 6;
    QHash<quint64, QIcon> m_rowIcons;
    QRect m_anchor;
    bool m_above = true;
    bool m_modeMenu = false;
    bool m_shadowRemovalQueued = false;
};

// Identify keyboard actions without treating projection, wheel or groove changes as seeks.
class SeekSlider final : public QSlider
{
public:
    explicit SeekSlider(QWidget *parent)
        : QSlider(Qt::Horizontal, parent)
    {
    }

    [[nodiscard]] bool isHandlingKey() const
    {
        return m_handlingKey;
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->modifiers() != Qt::NoModifier
            && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
            event->ignore();
            return;
        }
        const QScopedValueRollback<bool> handlingKey(m_handlingKey, true);
        QSlider::keyPressEvent(event);
    }

private:
    bool m_handlingKey = false;
};

// The artwork remains a passive shared view; only the footer adds this local
// focusable overlay. Its transparent idle state leaves the existing image intact.
class CoverDetailButton final : public QPushButton
{
public:
    explicit CoverDetailButton(QWidget *parent)
        : QPushButton(parent)
    {
        // Override the shared opaque QPushButton background locally; the
        // parent cover must still be repainted when this overlay disappears.
        setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: none; padding: 0; }"));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAutoDefault(false);
        setDetailVisible(false);
    }

    void setDetailVisible(bool visible)
    {
        m_detailVisible = visible;
        const QString action = visible ? QStringLiteral("收起播放详情")
                                       : QStringLiteral("展开播放详情");
        setToolTip(action);
        setAccessibleName(action);
        update();
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter) m_hovered = true;
        else if (event->type() == QEvent::Leave) m_hovered = false;
        const bool handled = QPushButton::event(event);
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave
            || event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut
            || event->type() == QEvent::EnabledChange)
            update();
        return handled;
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        // Unlike the transport buttons, this navigation entry also accepts
        // Enter without becoming a dialog-wide default button.
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && event->modifiers() == Qt::NoModifier) {
            event->accept();
            if (isEnabled() && !event->isAutoRepeat()) click();
            return;
        }
        QPushButton::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        if (!isEnabled() || (!m_hovered && !hasFocus() && !isDown())) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds(rect());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, isDown() ? 160 : 120));
        painter.drawRoundedRect(bounds, 8, 8);
        const QPointF center = bounds.center();
        const qreal direction = m_detailVisible ? -1.0 : 1.0;
        painter.setPen(QPen(QColor("#F4FFF9"), 2.3, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(center + QPointF(-8, 3 * direction),
                         center + QPointF(0, -4 * direction));
        painter.drawLine(center + QPointF(0, -4 * direction),
                         center + QPointF(8, 3 * direction));
        if (hasFocus() && property("buttonKeyboardFocus").toBool()) {
            painter.setPen(QPen(QColor("#69C8A5"), 2));
            painter.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 7, 7);
        }
    }

private:
    bool m_detailVisible = false;
    bool m_hovered = false;
};

class ElidingAudioNameLabel final : public QLabel
{
public:
    explicit ElidingAudioNameLabel(QWidget *parent)
        : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(0);
    }

    void setAudioName(const QString &name)
    {
        m_fullName = name;
        setToolTip(m_fullName);
        setAccessibleDescription(m_fullName);
        updateDisplayedText();
    }

    QSize sizeHint() const override
    {
        return QSize(240, QLabel::sizeHint().height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, QLabel::minimumSizeHint().height());
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateDisplayedText();
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange
            || event->type() == QEvent::ApplicationFontChange
            || event->type() == QEvent::StyleChange) {
            updateDisplayedText();
        }
    }

private:
    void updateDisplayedText()
    {
        setText(fontMetrics().elidedText(m_fullName, Qt::ElideMiddle,
                                        contentsRect().width()));
    }

    QString m_fullName;
};

} // namespace

PlaybackControlsWidget::PlaybackControlsWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground);
    m_identity = new QWidget(this);
    m_identity->setObjectName(QStringLiteral("playbackIdentity"));
    m_identity->setMinimumWidth(0);
    m_identity->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *cover = new CoverArtWidget(m_identity);
    m_coverLabel = cover;
    cover->setObjectName(QStringLiteral("playbackCoverPlaceholder"));
    m_coverButton = new CoverDetailButton(cover);
    m_coverButton->setObjectName(QStringLiteral("togglePlaybackDetailButton"));
    m_coverButton->setGeometry(cover->rect());
    m_coverButton->setEnabled(false);
    cover->installEventFilter(this);

    m_audioNameLabel = new ElidingAudioNameLabel(m_identity);
    m_audioNameLabel->setObjectName(QStringLiteral("audioNameLabel"));
    m_audioNameLabel->setAccessibleName(QStringLiteral("当前音频"));
    QFont audioFont = m_audioNameLabel->font();
    audioFont.setWeight(QFont::DemiBold);
    m_audioNameLabel->setFont(audioFont);
    m_workNameLabel = new ElidingAudioNameLabel(m_identity);
    m_workNameLabel->setObjectName(QStringLiteral("workNameLabel"));
    m_workNameLabel->setAccessibleName(QStringLiteral("当前音频所属作品"));
    m_workNameLabel->setProperty("secondaryText", true);
    setAudioName(QString());
    setWorkName(QString());
    auto *titles = new QVBoxLayout;
    titles->setSpacing(5);
    titles->addWidget(m_audioNameLabel);
    titles->addWidget(m_workNameLabel);
    auto *identityLayout = new QHBoxLayout(m_identity);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(12);
    identityLayout->addWidget(cover);
    identityLayout->addLayout(titles, 1);

    m_transport = new QWidget(this);
    m_transport->setObjectName(QStringLiteral("playbackTransport"));
    m_playPauseButton = new QPushButton(m_transport);
    m_playPauseButton->setObjectName(QStringLiteral("playPauseButton"));
    m_playPauseButton->setEnabled(false);
    m_playPauseButton->setProperty("primaryAction", true);
    m_playPauseButton->setFixedSize(48, 48);
    m_playPauseButton->setIconSize(QSize(24, 24));
    updatePlayAction(false);
    m_playbackModeButton = new QPushButton(m_transport);
    m_playbackModeButton->setObjectName(QStringLiteral("playbackModeButton"));
    m_volumeButton = new QPushButton(m_transport);
    m_volumeButton->setObjectName(QStringLiteral("playbackVolumeButton"));
    m_playbackModePopup = new PlaybackPopupMenu(this, true);
    m_playbackModePopup->setObjectName(QStringLiteral("playbackModeMenu"));
    m_playbackModePopup->setAttribute(Qt::WA_NoMouseReplay);
    m_playbackModePopup->installEventFilter(this);
    auto *modeGroup = new QActionGroup(m_playbackModePopup);
    modeGroup->setExclusive(true);
    const PlaybackMode modes[] = {PlaybackMode::Shuffle, PlaybackMode::Sequential,
                                  PlaybackMode::RepeatOne, PlaybackMode::RepeatAll};
    const char *actionNames[] = {"playbackModeShuffleAction", "playbackModeSequentialAction",
                                 "playbackModeRepeatOneAction", "playbackModeRepeatAllAction"};
    int modeIndex = 0;
    for (const PlaybackMode mode : modes) {
        auto *action = m_playbackModePopup->addAction(barIcon(modeIcon(mode)), modeName(mode));
        action->setObjectName(QString::fromLatin1(actionNames[modeIndex++]));
        action->setCheckable(true);
        action->setData(static_cast<int>(mode));
        modeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            // A menu selection is an intention, not an optimistic model write.
            setPlaybackMode(m_playbackMode);
            emit playbackModeRequested(mode);
        });
        if (mode != PlaybackMode::RepeatAll)
            m_playbackModePopup->addSeparator();
    }
    setPlaybackMode(PlaybackMode::Sequential);

    m_volumePopup = new PlaybackPopupMenu(this, false);
    m_volumePopup->setObjectName(QStringLiteral("playbackVolumeMenu"));
    m_volumePopup->setFixedWidth(72);
    m_volumePopup->setAttribute(Qt::WA_NoMouseReplay);
    m_volumePopup->installEventFilter(this);
    auto *volumeAction = new QWidgetAction(m_volumePopup);
    auto *volumeContent = new QWidget(m_volumePopup);
    volumeContent->setObjectName(QStringLiteral("playbackVolumeContent"));
    auto *volumeLayout = new QVBoxLayout(volumeContent);
    volumeLayout->setContentsMargins(0, 12, 0, 6);
    volumeLayout->setSpacing(10);
    m_volumeSlider = new QSlider(Qt::Vertical, volumeContent);
    m_volumeSlider->setObjectName(QStringLiteral("volumeSlider"));
    m_volumeSlider->setAccessibleName(QStringLiteral("音量"));
    m_volumeSlider->setToolTip(QStringLiteral("音量"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(70);
    m_volumeSlider->setSingleStep(1);
    m_volumeSlider->setPageStep(10);
    m_volumeSlider->setFixedHeight(140);
    m_volumeSlider->setFixedWidth(28);
    m_volumeSlider->installEventFilter(this);
    m_volumePercentLabel = new QLabel(volumeContent);
    m_volumePercentLabel->setObjectName(QStringLiteral("playbackVolumePercentLabel"));
    m_volumePercentLabel->setTextFormat(Qt::PlainText);
    m_volumePercentLabel->setAlignment(Qt::AlignCenter);
    m_volumePercentLabel->setAccessibleName(QStringLiteral("当前音量"));
    m_muteButton = new QPushButton(volumeContent);
    m_muteButton->setObjectName(QStringLiteral("playbackMuteButton"));
    m_muteButton->setProperty("playbackPopupAction", true);
    m_muteButton->setFixedSize(32, 32);
    m_muteButton->setIconSize(QSize(24, 24));
    m_muteButton->installEventFilter(this);
    volumeLayout->addWidget(m_volumeSlider, 0, Qt::AlignHCenter);
    volumeLayout->addWidget(m_volumePercentLabel);
    auto *volumeSeparator = new QFrame(volumeContent);
    volumeSeparator->setObjectName(QStringLiteral("playbackVolumeSeparator"));
    volumeSeparator->setFrameShape(QFrame::HLine);
    volumeLayout->addWidget(volumeSeparator);
    volumeLayout->addWidget(m_muteButton, 0, Qt::AlignHCenter);
    volumeAction->setDefaultWidget(volumeContent);
    m_volumePopup->addAction(volumeAction);
    setTabOrder(m_volumeSlider, m_muteButton);
    updateVolumeDisplay();
    m_previousButton = new QPushButton(m_transport);
    m_previousButton->setObjectName(QStringLiteral("previousTrackButton"));
    m_previousButton->setIcon(barIcon(BarIcon::Previous));
    m_previousButton->setAccessibleName(QStringLiteral("上一首"));
    m_previousButton->setToolTip(QStringLiteral("播放队列中的上一首"));
    m_nextButton = new QPushButton(m_transport);
    m_nextButton->setObjectName(QStringLiteral("nextTrackButton"));
    m_nextButton->setIcon(barIcon(BarIcon::Next));
    m_nextButton->setAccessibleName(QStringLiteral("下一首"));
    m_nextButton->setToolTip(QStringLiteral("播放队列中的下一首"));
    for (auto *button : {m_playbackModeButton, m_previousButton, m_nextButton, m_volumeButton}) {
        button->setFixedSize(32, 32);
        button->setIconSize(QSize(22, 22));
        button->setProperty("playbackTransportAction", true);
    }
    m_playbackModeButton->setIconSize(QSize(24, 24));
    m_volumeButton->setIconSize(QSize(24, 24));
    setQueueNavigationAvailable(false, false);
    auto *transportRow = new QHBoxLayout(m_transport);
    transportRow->setContentsMargins(0, 0, 0, 0);
    transportRow->setSpacing(10);
    transportRow->addStretch();
    transportRow->addWidget(m_playbackModeButton);
    transportRow->addWidget(m_previousButton);
    transportRow->addWidget(m_playPauseButton);
    transportRow->addWidget(m_nextButton);
    transportRow->addWidget(m_volumeButton);
    transportRow->addStretch();

    m_timeline = new QWidget(this);
    m_timeline->setObjectName(QStringLiteral("playbackTimeline"));
    m_timeline->setMinimumWidth(0);
    m_seekSlider = new SeekSlider(m_timeline);
    m_seekSlider->setObjectName(QStringLiteral("seekSlider"));
    m_seekSlider->setAccessibleName(QStringLiteral("播放进度"));
    m_seekSlider->setMinimumWidth(24);
    m_seekSlider->setRange(0, 0);
    m_seekSlider->setSingleStep(5000);
    m_seekSlider->setPageStep(10000);
    m_seekSlider->installEventFilter(this);
    m_seekSlider->setEnabled(false);
    m_timeLabel = new QLabel(QStringLiteral("0:00"), m_timeline);
    m_timeLabel->setObjectName(QStringLiteral("timeLabel"));
    m_timeLabel->setAccessibleName(QStringLiteral("当前位置"));
    m_durationLabel = new QLabel(QStringLiteral("0:00"), m_timeline);
    m_durationLabel->setObjectName(QStringLiteral("durationLabel"));
    m_durationLabel->setAccessibleName(QStringLiteral("总时长"));
    for (auto *label : {m_timeLabel, m_durationLabel}) {
        label->setTextFormat(Qt::PlainText);
        label->setProperty("secondaryText", true);
        label->setMinimumWidth(32);
        label->setMaximumWidth(112);
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    m_timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_durationLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *timelineRow = new QHBoxLayout(m_timeline);
    timelineRow->setContentsMargins(0, 0, 0, 0);
    timelineRow->setSpacing(8);
    timelineRow->addWidget(m_timeLabel);
    timelineRow->addWidget(m_seekSlider, 1);
    timelineRow->addWidget(m_durationLabel);

    m_auxiliary = new QWidget(this);
    m_auxiliary->setObjectName(QStringLiteral("playbackAuxiliary"));
    m_selectSubtitleButton = new QPushButton(m_auxiliary);
    m_selectSubtitleButton->setObjectName(QStringLiteral("selectPlaybackSubtitleButton"));
    m_selectSubtitleButton->setIcon(barIcon(BarIcon::Subtitles));
    m_selectSubtitleButton->setAccessibleName(QStringLiteral("选择或管理字幕"));
    m_selectSubtitleButton->setToolTip(QStringLiteral("选择或管理字幕"));
    m_selectSubtitleButton->setEnabled(false);
    m_toggleSubtitlePanelButton = new QPushButton(m_auxiliary);
    m_toggleSubtitlePanelButton->setObjectName(QStringLiteral("toggleSubtitlePanelButton"));
    m_toggleSubtitlePanelButton->setCheckable(true);
    m_toggleSubtitlePanelButton->setAccessibleName(QStringLiteral("字幕面板"));
    setSubtitlePanelVisible(false);
    m_toggleFloatingButton = new QPushButton(m_auxiliary);
    m_toggleFloatingButton->setObjectName(QStringLiteral("togglePlaybackFloatingSubtitleButton"));
    m_toggleFloatingButton->setCheckable(true);
    m_toggleFloatingButton->setIcon(barIcon(BarIcon::Desktop));
    m_toggleFloatingButton->setAccessibleName(QStringLiteral("桌面字幕"));
    setFloatingSubtitleVisible(false);
    m_toggleQueueButton = new QPushButton(m_auxiliary);
    m_toggleQueueButton->setObjectName(QStringLiteral("togglePlaybackQueueButton"));
    m_toggleQueueButton->setCheckable(true);
    m_toggleQueueButton->setFixedSize(52, 32);
    m_toggleQueueButton->setIconSize(QSize(20, 20));
    m_toggleQueueButton->setProperty("playbackQueueAction", true);
    setQueueState(0, false);
    for (auto *button : {m_selectSubtitleButton, m_toggleFloatingButton,
                         m_toggleSubtitlePanelButton}) {
        button->setFixedSize(32, 32);
        button->setIconSize(QSize(22, 22));
        button->setProperty("playbackAuxiliary", true);
    }
    auto *auxiliaryRow = new QHBoxLayout(m_auxiliary);
    auxiliaryRow->setContentsMargins(0, 0, 0, 0);
    auxiliaryRow->setSpacing(6);
    auxiliaryRow->addWidget(m_selectSubtitleButton);
    auxiliaryRow->addWidget(m_toggleFloatingButton);
    auxiliaryRow->addWidget(m_toggleSubtitlePanelButton);
    auxiliaryRow->addWidget(m_toggleQueueButton);

    m_barLayout = new QGridLayout;
    m_barLayout->setHorizontalSpacing(22);
    m_barLayout->setVerticalSpacing(5);
    m_barLayout->addWidget(m_identity, 0, 0, 2, 1);
    m_barLayout->addWidget(m_transport, 0, 1);
    m_barLayout->addWidget(m_timeline, 1, 1);
    m_barLayout->addWidget(m_auxiliary, 0, 2, 2, 1);
    m_barLayout->setColumnStretch(0, 3);
    m_barLayout->setColumnStretch(1, 5);
    m_barLayout->setColumnStretch(2, 2);
    auto *layout = new QVBoxLayout(this);
    // A previous wide grid must not clamp the resize before compact placement.
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(8);
    layout->addLayout(m_barLayout);

    setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(m_playbackModeButton);
    setTabOrder(m_coverButton, m_playbackModeButton);
    setTabOrder(m_playbackModeButton, m_previousButton);
    setTabOrder(m_previousButton, m_playPauseButton);
    setTabOrder(m_playPauseButton, m_nextButton);
    setTabOrder(m_nextButton, m_volumeButton);
    setTabOrder(m_volumeButton, m_seekSlider);
    setTabOrder(m_seekSlider, m_selectSubtitleButton);
    setTabOrder(m_selectSubtitleButton, m_toggleFloatingButton);
    setTabOrder(m_toggleFloatingButton, m_toggleSubtitlePanelButton);
    setTabOrder(m_toggleSubtitlePanelButton, m_toggleQueueButton);

    connect(m_selectSubtitleButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::selectSubtitleRequested);
    connect(m_coverButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::togglePlaybackDetailRequested);
    connect(m_toggleSubtitlePanelButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::toggleSubtitlePanelRequested);
    connect(m_toggleFloatingButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::toggleFloatingSubtitleRequested);
    connect(m_previousButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::previousRequested);
    connect(m_nextButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::nextRequested);
    connect(m_toggleQueueButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::toggleQueueRequested);
    connect(m_playPauseButton, &QPushButton::clicked,
            this, &PlaybackControlsWidget::playPauseRequested);
    connect(m_playbackModeButton, &QPushButton::clicked, this, [this] {
        togglePopup(m_playbackModePopup, m_playbackModeButton);
    });
    connect(m_volumeButton, &QPushButton::clicked, this, [this] {
        togglePopup(m_volumePopup, m_volumeButton);
    });
    connect(m_muteButton, &QPushButton::clicked, this, [this] {
        m_volumeSlider->setValue(m_volumeSlider->value() > 0 ? 0 : m_lastNonzeroVolume);
    });
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int volume) {
        updateVolumeDisplay();
        emit volumeChangeRequested(volume);
    });
    connect(m_seekSlider, &QSlider::actionTriggered, this, [this](int) {
        if (!static_cast<SeekSlider *>(m_seekSlider)->isHandlingKey()
            || !canSeek() || m_seekSlider->isSliderDown()) {
            return;
        }
        const qint64 requestedPosition = m_seekSlider->sliderPosition();
        updateTimeDisplay(requestedPosition);
        emit seekRequested(requestedPosition);
    });
    connect(m_seekSlider, &QSlider::sliderPressed, this, [this]() {
        m_userSeeking = canSeek();
    });
    connect(m_seekSlider, &QSlider::sliderMoved, this, [this](int position) {
        if (m_userSeeking && canSeek())
            updateTimeDisplay(position);
    });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]() {
        const bool shouldSeek = m_userSeeking && canSeek();
        m_userSeeking = false;
        if (!shouldSeek)
            return;
        const qint64 requestedPosition = m_seekSlider->sliderPosition();
        // End the drag before a synchronous backend notification can return.
        updateTimeDisplay(requestedPosition);
        emit seekRequested(requestedPosition);
    });
    // Moving the containing window does not send Move to its child widgets.
    window()->installEventFilter(this);
}

void PlaybackControlsWidget::setAudioName(const QString &name)
{
    static_cast<ElidingAudioNameLabel *>(m_audioNameLabel)->setAudioName(
        name.isEmpty() ? QStringLiteral("尚未播放音频") : name);
    m_workNameLabel->setVisible(!name.isEmpty());
}

void PlaybackControlsWidget::setWorkName(const QString &name)
{
    static_cast<ElidingAudioNameLabel *>(m_workNameLabel)->setAudioName(
        name.isEmpty() ? QStringLiteral("本地音频") : name);
}

void PlaybackControlsWidget::setCoverImage(const QImage &image, const QString &sourcePath, const QString &detail)
{
    m_coverLabel->setImage(image, sourcePath, detail);
}

void PlaybackControlsWidget::setCoverPlaceholder(const QString &message)
{
    m_coverLabel->clearImage(message);
}

void PlaybackControlsWidget::setPlaybackDetailVisible(bool visible)
{
    m_playbackDetailVisible = visible;
    static_cast<CoverDetailButton *>(m_coverButton)->setDetailVisible(visible);
    updateSubtitlePanelAction();
}

void PlaybackControlsWidget::setSubtitleSelectionAvailable(bool available)
{
    m_selectSubtitleButton->setEnabled(available);
}

void PlaybackControlsWidget::setSubtitlePanelVisible(bool visible)
{
    m_subtitlePanelVisible = visible;
    updateSubtitlePanelAction();
}

void PlaybackControlsWidget::updateSubtitlePanelAction()
{
    const bool visible = m_playbackDetailVisible || m_subtitlePanelVisible;
    const QSignalBlocker blocker(m_toggleSubtitlePanelButton);
    m_toggleSubtitlePanelButton->setEnabled(!m_playbackDetailVisible);
    m_toggleSubtitlePanelButton->setChecked(visible);
    m_toggleSubtitlePanelButton->setIcon(barIcon(BarIcon::Panel,
        QColor(visible ? "#69C8A5" : "#BDC4CE")));
    m_toggleSubtitlePanelButton->setToolTip(m_playbackDetailVisible
        ? QStringLiteral("播放详情中已显示字幕")
        : visible ? QStringLiteral("收起字幕面板") : QStringLiteral("展开字幕面板"));
}

void PlaybackControlsWidget::setFloatingSubtitleVisible(bool visible)
{
    const QSignalBlocker blocker(m_toggleFloatingButton);
    m_toggleFloatingButton->setChecked(visible);
    m_toggleFloatingButton->setIcon(barIcon(BarIcon::Desktop,
        QColor(visible ? "#69C8A5" : "#BDC4CE")));
    m_toggleFloatingButton->setToolTip(visible ? QStringLiteral("隐藏桌面字幕")
                                             : QStringLiteral("显示桌面字幕"));
}

void PlaybackControlsWidget::setQueueNavigationAvailable(bool previous, bool next)
{
    // Queue navigation remains usable after media failure and while a resume
    // operation is pending; the queue owner alone determines its availability.
    m_previousButton->setEnabled(previous);
    m_nextButton->setEnabled(next);
}

void PlaybackControlsWidget::setQueueState(int count, bool visible)
{
    const QSignalBlocker blocker(m_toggleQueueButton);
    count = std::max(0, count);
    m_toggleQueueButton->setChecked(visible);
    m_toggleQueueButton->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
    m_toggleQueueButton->setAccessibleName(QStringLiteral("播放队列（%1 项）").arg(count));
    m_toggleQueueButton->setToolTip((visible ? QStringLiteral("收起播放队列（%1 项）")
                                           : QStringLiteral("展开播放队列（%1 项）")).arg(count));
    m_toggleQueueButton->setIcon(barIcon(BarIcon::Queue,
        QColor(visible ? "#69C8A5" : "#BDC4CE")));
}

void PlaybackControlsWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    hidePopups();
    updateBarLayout();
}

void PlaybackControlsWidget::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    hidePopups();
}

void PlaybackControlsWidget::hideEvent(QHideEvent *event)
{
    hidePopups();
    QWidget::hideEvent(event);
}

void PlaybackControlsWidget::hidePopups()
{
    for (auto *popup : {m_playbackModePopup, m_volumePopup})
        if (popup) popup->hide();
}

void PlaybackControlsWidget::updateBarLayout()
{
    const int mode = width() < 540 ? 0 : width() < 680 ? 1 : 2;
    if (m_barLayoutMode == mode)
        return;
    m_barLayoutMode = mode;
    m_coverLabel->setFixedSize(mode == 0 ? 48 : 64, mode == 0 ? 48 : 64);
    m_coverButton->setGeometry(m_coverLabel->rect());
    for (auto *widget : {m_identity, m_transport, m_timeline, m_auxiliary})
        m_barLayout->removeWidget(widget);
    if (mode == 0) {
        m_barLayout->setHorizontalSpacing(12);
        m_barLayout->addWidget(m_identity, 0, 0);
        m_barLayout->addWidget(m_transport, 0, 1);
        m_barLayout->addWidget(m_timeline, 1, 0, 1, 2);
        m_barLayout->addWidget(m_auxiliary, 2, 0, 1, 2);
        m_barLayout->setColumnStretch(0, 1);
        m_barLayout->setColumnStretch(1, 0);
        m_barLayout->setColumnStretch(2, 0);
    } else if (mode == 1) {
        m_barLayout->setHorizontalSpacing(12);
        m_barLayout->addWidget(m_identity, 0, 0);
        m_barLayout->addWidget(m_transport, 0, 1);
        m_barLayout->addWidget(m_timeline, 1, 0);
        m_barLayout->addWidget(m_auxiliary, 1, 1);
        m_barLayout->setColumnStretch(0, 1);
        m_barLayout->setColumnStretch(1, 0);
        m_barLayout->setColumnStretch(2, 0);
    } else {
        m_barLayout->setHorizontalSpacing(22);
        m_barLayout->addWidget(m_identity, 0, 0, 2, 1);
        m_barLayout->addWidget(m_transport, 0, 1);
        m_barLayout->addWidget(m_timeline, 1, 1);
        m_barLayout->addWidget(m_auxiliary, 0, 2, 2, 1);
        m_barLayout->setColumnStretch(0, 3);
        m_barLayout->setColumnStretch(1, 5);
        m_barLayout->setColumnStretch(2, 2);
    }
    updateGeometry();
}

void PlaybackControlsWidget::updatePlayAction(bool playing)
{
    m_playPauseButton->setIcon(barIcon(playing ? BarIcon::Pause : BarIcon::Play,
                                      QColor("#F4FFF9")));
    m_playPauseButton->setAccessibleName(playing ? QStringLiteral("暂停")
                                               : QStringLiteral("播放"));
    m_playPauseButton->setToolTip(playing ? QStringLiteral("暂停当前音频")
                                        : QStringLiteral("播放当前音频"));
}

void PlaybackControlsWidget::setMediaAvailable(bool available)
{
    m_mediaAvailable = available;
    m_playPauseButton->setEnabled(available && !m_resumePending);
    m_coverButton->setEnabled(available);
    if (!available) {
        setResumePending(false);
        cancelSeekDrag();
        m_positionMs = 0;
        m_durationMs = 0;
        m_seekable = false;
        updatePlayAction(false);
    }
    updateSeekControls();
}

void PlaybackControlsWidget::setPosition(qint64 positionMs)
{
    if (!m_mediaAvailable)
        return;
    m_positionMs = std::max<qint64>(0, positionMs);
    if (!m_seekSlider->isSliderDown()) {
        const QSignalBlocker blocker(m_seekSlider);
        m_seekSlider->setValue(static_cast<int>(std::min<qint64>(
            m_positionMs, m_seekSlider->maximum())));
        updateTimeDisplay(m_positionMs);
    }
}

void PlaybackControlsWidget::setDuration(qint64 durationMs)
{
    if (!m_mediaAvailable)
        return;
    m_durationMs = std::max<qint64>(0, durationMs);
    updateSeekControls();
}

void PlaybackControlsWidget::setSeekable(bool seekable)
{
    if (!m_mediaAvailable)
        return;
    m_seekable = seekable;
    updateSeekControls();
}

void PlaybackControlsWidget::setPlaying(bool playing)
{
    if (!m_mediaAvailable)
        return;
    updatePlayAction(playing);
}

void PlaybackControlsWidget::setVolumePercent(int volume)
{
    const QSignalBlocker blocker(m_volumeSlider);
    m_volumeSlider->setValue(std::clamp(volume, 0, 100));
    updateVolumeDisplay();
}

void PlaybackControlsWidget::setPlaybackMode(PlaybackMode mode)
{
    switch (mode) {
    case PlaybackMode::Sequential:
    case PlaybackMode::RepeatAll:
    case PlaybackMode::RepeatOne:
    case PlaybackMode::Shuffle:
        break;
    default:
        mode = PlaybackMode::Sequential;
    }
    m_playbackMode = mode;
    m_playbackModeButton->setIcon(barIcon(modeIcon(mode)));
    m_playbackModeButton->setAccessibleName(QStringLiteral("播放模式：%1").arg(modeName(mode)));
    m_playbackModeButton->setToolTip(QStringLiteral("播放模式：%1（点击选择）").arg(modeName(mode)));
    for (auto *action : m_playbackModePopup->actions()) {
        if (action->isSeparator()) continue;
        const QSignalBlocker blocker(action);
        action->setChecked(action->data().toInt() == static_cast<int>(mode));
    }
}

void PlaybackControlsWidget::updateVolumeDisplay()
{
    const int volume = m_volumeSlider->value();
    if (volume > 0) m_lastNonzeroVolume = volume;
    const auto symbol = volume > 0 ? BarIcon::Volume : BarIcon::Muted;
    m_volumeButton->setIcon(barIcon(symbol));
    m_volumeButton->setAccessibleName(QStringLiteral("音量：%1%").arg(volume));
    m_volumeButton->setToolTip(QStringLiteral("音量：%1%（点击调节）").arg(volume));
    m_volumePercentLabel->setText(QStringLiteral("%1%").arg(volume));
    m_muteButton->setIcon(barIcon(symbol, QColor(QStringLiteral("#1ED760"))));
    m_muteButton->setAccessibleName(volume > 0 ? QStringLiteral("静音") : QStringLiteral("取消静音"));
    m_muteButton->setToolTip(volume > 0 ? QStringLiteral("静音")
                                      : QStringLiteral("取消静音，恢复至 %1%").arg(m_lastNonzeroVolume));
}

void PlaybackControlsWidget::togglePopup(QMenu *popup, QPushButton *opener)
{
    if (popup->isVisible()) {
        popup->hide();
        opener->setFocus(Qt::PopupFocusReason);
        return;
    }
    for (auto *other : {m_playbackModePopup, m_volumePopup})
        if (other != popup) other->hide();
    opener->setFocus(Qt::PopupFocusReason);
    const QRect anchor(opener->mapToGlobal(QPoint()), opener->size());
    QScreen *screen = QGuiApplication::screenAt(anchor.center());
    if (!screen) screen = opener->screen();
    static_cast<PlaybackPopupMenu *>(popup)->popupAt(anchor, screen->availableGeometry());
    if (popup == m_volumePopup) {
        m_volumeSlider->setFocus(Qt::PopupFocusReason);
    } else {
        QAction *active = nullptr;
        for (auto *action : popup->actions()) {
            if (!action->isEnabled() || action->isSeparator()) continue;
            if (!active || action->isChecked()) active = action;
        }
        popup->setActiveAction(active);
    }
}

bool PlaybackControlsWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_coverLabel && event->type() == QEvent::Resize)
        m_coverButton->setGeometry(m_coverLabel->rect());
    if (watched == window() && (event->type() == QEvent::Move
        || event->type() == QEvent::Resize || event->type() == QEvent::Hide))
        hidePopups();
    // Let QMenu handle Escape, including its popup stack and focus cleanup.
    // Hiding it here can race that cleanup and lose the opener's focus.
    if (watched == m_seekSlider && !m_seekSlider->isSliderDown()
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
            || event->type() == QEvent::ShortcutOverride)) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
            event->accept();
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()
                && m_seekSlider->isEnabled() && m_mediaAvailable && !m_resumePending
                && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()
                && (!QApplication::activeWindow() || QApplication::activeWindow() == window()))
                emit playPauseRequested();
            return true;
        }
    }
    if ((watched == m_playbackModePopup || watched == m_volumePopup)
        && event->type() == QEvent::MouseButtonPress) {
        auto *popup = static_cast<QMenu *>(watched);
        const QPoint point = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
        if (!popup->frameGeometry().contains(point)) {
            for (auto *opener : {m_playbackModeButton, m_volumeButton}) {
                const QRect rect(opener->mapToGlobal(QPoint()), opener->size());
                if (!rect.contains(point)) continue;
                popup->hide();
                if (!opener->isEnabled()) return true;
                opener->setFocus(Qt::MouseFocusReason);
                auto *target = opener == m_volumeButton ? m_volumePopup : m_playbackModePopup;
                if (target != popup) {
                    QTimer::singleShot(0, this, [this, target, opener] {
                        if (opener->isVisible() && opener->isEnabled()) togglePopup(target, opener);
                    });
                }
                return true;
            }
        }
    }
    if ((watched == m_volumeSlider || watched == m_muteButton)
        && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            (watched == m_volumeSlider ? static_cast<QWidget *>(m_muteButton)
                                      : static_cast<QWidget *>(m_volumeSlider))->setFocus(
                key->key() == Qt::Key_Backtab ? Qt::BacktabFocusReason : Qt::TabFocusReason);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PlaybackControlsWidget::setResumePending(bool pending)
{
    m_resumePending = pending;
    m_playPauseButton->setEnabled(m_mediaAvailable && !pending);
}

bool PlaybackControlsWidget::canSeek() const
{
    return m_mediaAvailable && m_seekable && m_durationMs > 0;
}

void PlaybackControlsWidget::updateSeekControls()
{
    const bool enabled = canSeek();
    if (!enabled)
        cancelSeekDrag();
    const QSignalBlocker blocker(m_seekSlider);
    const qint64 maximum = std::min<qint64>(m_durationMs,
                                           std::numeric_limits<int>::max());
    m_seekSlider->setRange(0, static_cast<int>(maximum));
    m_seekSlider->setEnabled(enabled);
    if (m_userSeeking && m_seekSlider->isSliderDown()) {
        // Duration changes may clamp the preview, but never replace it with the
        // latest media position until this drag finishes or is cancelled.
        updateTimeDisplay(m_seekSlider->sliderPosition());
    } else {
        m_seekSlider->setValue(static_cast<int>(std::min(m_positionMs, maximum)));
        updateTimeDisplay(m_positionMs);
    }
}

void PlaybackControlsWidget::cancelSeekDrag()
{
    m_userSeeking = false;
    const QSignalBlocker blocker(m_seekSlider);
    m_seekSlider->setSliderDown(false);
}

void PlaybackControlsWidget::updateTimeDisplay(qint64 positionMs)
{
    m_timeLabel->setText(formatTime(positionMs));
    m_durationLabel->setText(formatTime(m_durationMs));
    m_timeLabel->setToolTip(m_timeLabel->text());
    m_durationLabel->setToolTip(m_durationLabel->text());
}

} // namespace asmr
