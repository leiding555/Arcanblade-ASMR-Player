#include "TitleBarWidget.h"
#include "ui/ButtonFocusController.h"

#include <QEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QtMath>

namespace asmr {
namespace {

enum class WindowSymbol { Minimize, Maximize, Restore, Close };
constexpr int BrandIconSize = 28;

// Drawing in widget coordinates lets Qt handle fractional display scaling.
// The style still paints hover, pressed, focus and disabled backgrounds.
class WindowControlButton final : public QToolButton {
public:
    WindowControlButton(WindowSymbol symbol, int height, QWidget *parent)
        : QToolButton(parent), m_symbol(symbol)
    {
        setFixedSize(48, height);
        setFocusPolicy(Qt::StrongFocus);
        setAutoRaise(false);
    }

    void setSymbol(WindowSymbol symbol)
    {
        m_symbol = symbol;
        update();
    }

    void setWindowActive(bool active)
    {
        m_active = active;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor foreground = !isEnabled() ? QColor(105, 113, 125)
            : m_active ? QColor(224, 230, 237) : QColor(151, 160, 173);
        painter.setPen(QPen(foreground, 1.4, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
        painter.translate(QPointF(width() / 2.0, height() / 2.0));
        switch (m_symbol) {
        case WindowSymbol::Minimize:
            painter.drawLine(QPointF(-6.0, 0.0), QPointF(6.0, 0.0));
            break;
        case WindowSymbol::Maximize:
            painter.drawRect(QRectF(-5.5, -5.5, 11.0, 11.0));
            break;
        case WindowSymbol::Restore:
            painter.drawPolyline(QPolygonF{QPointF(-2.5, -5.5), QPointF(5.5, -5.5),
                                           QPointF(5.5, 2.5), QPointF(3.0, 2.5)});
            painter.drawRect(QRectF(-5.5, -2.5, 8.5, 8.0));
            break;
        case WindowSymbol::Close:
            painter.drawLine(QPointF(-5.0, -5.0), QPointF(5.0, 5.0));
            painter.drawLine(QPointF(-5.0, 5.0), QPointF(5.0, -5.0));
            break;
        }
    }

private:
    WindowSymbol m_symbol;
    bool m_active = true;
};

void setControlDescription(QToolButton *button, const QString &text)
{
    button->setToolTip(text);
    button->setAccessibleName(text);
}

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

TitleBarWidget::TitleBarWidget(QWidget *parent, Mode mode)
    : QWidget(parent), m_mode(mode)
{
    // Every player window using this bar shares the same button presentation,
    // including Qt-created message-box and dialog-button-box controls.
    ButtonFocusController::install(parent ? parent : this);
    const bool dialog = mode == Mode::Dialog;
    const int barHeight = dialog ? DialogHeight : Height;
    setObjectName(dialog ? QStringLiteral("dialogTitleBar") : QStringLiteral("mainTitleBar"));
    setFixedHeight(barHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_StyledBackground);
    setAccessibleName(QStringLiteral("窗口标题栏"));

    m_title = new QLabel(m_titleText, this);
    m_title->setObjectName(dialog ? QStringLiteral("dialogWindowTitle") : QStringLiteral("mainWindowTitle"));
    m_title->setTextFormat(Qt::PlainText);
    // A longer closing caption must shrink inside its slot on small screens,
    // rather than increasing the window minimum width or moving its controls.
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    if (!dialog) {
        m_applicationIcon = new QLabel(this);
        m_applicationIcon->setObjectName(QStringLiteral("mainWindowApplicationIcon"));
        // The icon and caption share an anchored group outside the controls'
        // layout. Their logical dimensions remain independent of display DPI.
        m_applicationIcon->setFixedSize(BrandIconSize, BrandIconSize);
        m_applicationIcon->setAlignment(Qt::AlignCenter);
        m_applicationIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_applicationIcon->setFocusPolicy(Qt::NoFocus);
        m_applicationIcon->setTextInteractionFlags(Qt::NoTextInteraction);
        updateApplicationIcon();
    }

    m_minimize = new WindowControlButton(WindowSymbol::Minimize, barHeight, this);
    m_maximize = new WindowControlButton(WindowSymbol::Maximize, barHeight, this);
    m_close = new WindowControlButton(WindowSymbol::Close, barHeight, this);
    m_minimize->setObjectName(QStringLiteral("windowMinimizeButton"));
    m_maximize->setObjectName(QStringLiteral("windowMaximizeButton"));
    m_close->setObjectName(QStringLiteral("windowCloseButton"));
    m_minimize->setProperty("windowControl", true);
    m_maximize->setProperty("windowControl", true);
    m_maximize->setProperty("nativeHovered", false);
    m_close->setProperty("windowControl", true);
    setControlDescription(m_minimize, QStringLiteral("最小化"));
    setControlDescription(m_maximize, QStringLiteral("最大化"));
    setControlDescription(m_close, QStringLiteral("关闭"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(dialog ? 20 : 0, 0, 0, 0);
    layout->setSpacing(0);
    if (dialog) layout->addWidget(m_title, 1);
    else layout->addStretch();
    layout->addWidget(m_minimize);
    layout->addWidget(m_maximize);
    layout->addWidget(m_close);
    m_minimize->setVisible(!dialog);
    m_maximize->setVisible(!dialog);
    m_minimize->setEnabled(!dialog);
    m_maximize->setEnabled(!dialog);

    connect(m_minimize, &QToolButton::clicked, this, &TitleBarWidget::minimizeRequested);
    connect(m_maximize, &QToolButton::clicked, this, &TitleBarWidget::maximizeRestoreRequested);
    connect(m_close, &QToolButton::clicked, this, &TitleBarWidget::closeRequested);
    updateBrandGeometry();
}

TitleBarWidget::Mode TitleBarWidget::mode() const { return m_mode; }
QToolButton *TitleBarWidget::minimizeButton() const { return m_minimize; }
QToolButton *TitleBarWidget::maximizeButton() const { return m_maximize; }
QToolButton *TitleBarWidget::closeButton() const { return m_close; }

bool TitleBarWidget::isDragPosition(const QPoint &localPosition) const
{
    if (m_closing || !isEnabled() || !rect().contains(localPosition))
        return false;
    for (const auto *button : {m_minimize, m_maximize, m_close}) {
        if (!button->isHidden() && button->geometry().contains(localPosition))
            return false;
    }
    return true;
}

void TitleBarWidget::setTitle(const QString &title)
{
    m_titleText = title;
    m_title->setText(m_closing ? title + QStringLiteral(" · 正在关闭") : title);
    updateBrandGeometry();
}

void TitleBarWidget::setBrandAlignmentRect(const QRect &rect)
{
    if (m_mode == Mode::Dialog || m_brandAlignmentRect == rect) return;
    m_brandAlignmentRect = rect;
    updateBrandGeometry();
}

void TitleBarWidget::setBrandIconCenterX(int logicalX)
{
    if (m_mode == Mode::Dialog || m_brandIconCenterX == logicalX) return;
    m_brandIconCenterX = logicalX;
    updateBrandGeometry();
}

void TitleBarWidget::updateBrandGeometry()
{
    if (m_mode != Mode::MainWindow || !m_applicationIcon || !m_minimize) return;
    // A standalone bar defaults to the usual sidebar span. Mapping and
    // clipping here also make narrow/closing captions independent of the
    // window's minimum size and right-hand controls.
    const QRect controlsFree(0, 0, qMax(0, width() - 3 * 48), height());
    const QRect requested = m_brandAlignmentRect.isValid()
        ? m_brandAlignmentRect : QRect(16, 0, 240, height());
    const QRect span = QRect(requested.x(), 0, requested.width(), height()).intersected(controlsFree);
    const QString caption = m_closing ? m_titleText + QStringLiteral(" · 正在关闭") : m_titleText;
    // Anchor the actual 28px icon, moving its caption by the same amount. The
    // fallback is only for standalone title bars; owners supply their styled
    // import icon's actual position, including compact-sidebar layout changes.
    const int iconCenterX = m_brandIconCenterX >= 0 ? m_brandIconCenterX : span.x() + 40;
    const int iconX = qBound(span.x(), iconCenterX - BrandIconSize / 2,
                            qMax(span.x(), span.x() + span.width() - BrandIconSize));
    constexpr int gap = 8;
    const int titleX = qMin(iconX + BrandIconSize + gap, span.x() + span.width());
    const int available = qMax(0, span.x() + span.width() - titleX);
    // Windows' native font engine can return a fractional advance even when
    // widget geometry is integral. Reserve its ceiling, and do not run an
    // already-fitting caption through an independently rounded elide path.
    const QFontMetricsF metrics(m_title->font());
    const int naturalTitleWidth = qCeil(metrics.horizontalAdvance(caption));
    const int titleWidth = qMin(naturalTitleWidth, available);
    const int centerY = height() / 2 + 9;
    const int textHeight = qMin(height(), m_title->fontMetrics().height());
    const int titleY = qBound(0, centerY - textHeight / 2, height() - textHeight);
    m_applicationIcon->setGeometry(iconX,
                                  centerY - BrandIconSize / 2, BrandIconSize, BrandIconSize);
    m_title->setGeometry(titleX, titleY, titleWidth, textHeight);
    m_title->setText(titleWidth >= naturalTitleWidth ? caption
        : metrics.elidedText(caption, Qt::ElideRight, titleWidth));
    m_title->setToolTip(caption);
    m_title->setAccessibleName(caption);
}

void TitleBarWidget::setTitleVisible(bool visible)
{
    m_title->setVisible(visible);
    if (m_applicationIcon) m_applicationIcon->setVisible(visible);
}

void TitleBarWidget::updateApplicationIcon()
{
    if (m_applicationIcon)
        m_applicationIcon->setPixmap(applicationMarkIcon().pixmap(
            QSize(BrandIconSize, BrandIconSize), devicePixelRatioF()));
}

bool TitleBarWidget::event(QEvent *event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::Show || event->type() == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        || event->type() == QEvent::DevicePixelRatioChange
#endif
        ) updateApplicationIcon();
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show
        || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
        || event->type() == QEvent::PolishRequest || event->type() == QEvent::LayoutRequest)
        updateBrandGeometry();
    return handled;
}

void TitleBarWidget::setMaximized(bool maximized)
{
    if (m_mode == Mode::Dialog || m_maximized == maximized)
        return;
    m_maximized = maximized;
    static_cast<WindowControlButton *>(m_maximize)->setSymbol(
        maximized ? WindowSymbol::Restore : WindowSymbol::Maximize);
    setControlDescription(m_maximize, maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
}

void TitleBarWidget::setWindowActive(bool active)
{
    setProperty("windowActive", active);
    for (auto *button : {m_minimize, m_maximize, m_close})
        static_cast<WindowControlButton *>(button)->setWindowActive(active);
    m_title->setProperty("windowActive", active);
    repolish(m_title);
}

void TitleBarWidget::setClosing(bool closing)
{
    m_closing = closing;
    m_title->setText(closing ? m_titleText + QStringLiteral(" · 正在关闭") : m_titleText);
    updateBrandGeometry();
    m_minimize->setEnabled(!closing && m_mode == Mode::MainWindow);
    m_maximize->setEnabled(!closing && m_mode == Mode::MainWindow);
    m_close->setEnabled(!closing);
    if (closing) {
        setNativeMaximizeHovered(false);
        setNativeMaximizePressed(false);
    }
}

void TitleBarWidget::setNativeMaximizeHovered(bool hovered)
{
    const bool effective = hovered && !m_closing && m_mode == Mode::MainWindow;
    if (m_maximize->property("nativeHovered").toBool() == effective)
        return;
    m_maximize->setProperty("nativeHovered", effective);
    repolish(m_maximize);
}

void TitleBarWidget::setNativeMaximizePressed(bool pressed)
{
    m_maximize->setDown(pressed && !m_closing && m_mode == Mode::MainWindow);
}

void TitleBarWidget::mousePressEvent(QMouseEvent *event)
{
    if (isDragPosition(event->position().toPoint())) {
        if (event->button() == Qt::LeftButton) {
            emit systemMoveRequested();
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton) {
            if (m_mode == Mode::MainWindow)
                emit systemMenuRequested(event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void TitleBarWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && isDragPosition(event->position().toPoint())) {
        if (m_mode == Mode::MainWindow)
            emit maximizeRestoreRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace asmr
