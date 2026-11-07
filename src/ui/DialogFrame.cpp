#include "ui/DialogFrame.h"

#include "ui/TitleBarWidget.h"
#include "ui/WindowFrameController.h"

#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QLayout>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace asmr {
namespace {
constexpr int Border = 1;

QSize withDecoration(const QSize &size)
{
    return QSize(std::min(QWIDGETSIZE_MAX, size.width() + Border * 2),
                 std::min(QWIDGETSIZE_MAX, size.height() + TitleBarWidget::DialogHeight + Border * 2));
}

QWidget *firstFocusable(QLayout *layout)
{
    for (int i = 0; i < layout->count(); ++i) {
        auto *item = layout->itemAt(i);
        if (auto *widget = item->widget()) {
            if (widget->isEnabled() && !widget->isHidden()
                && (widget->focusPolicy() & Qt::TabFocus)) return widget;
            if (auto *child = widget->layout())
                if (auto *focus = firstFocusable(child)) return focus;
        } else if (auto *child = item->layout()) {
            if (auto *focus = firstFocusable(child)) return focus;
        }
    }
    return nullptr;
}
} // namespace

DialogFrame::DialogFrame(QDialog *dialog, QLayout *contentLayout, bool resizable)
    : QObject(dialog)
{
    Q_ASSERT(dialog && contentLayout && !contentLayout->parent());
    Q_ASSERT(!dialog->layout());
    const QSize originalSize = dialog->size();
    const QSize originalMinimum = dialog->minimumSize();
    const QSize originalMaximum = dialog->maximumSize();
    const bool explicitlyResized = dialog->testAttribute(Qt::WA_Resized);
    m_originalMinimum = originalMinimum;
    m_autoSize = !explicitlyResized;
    QPointer<QWidget> initialFocus = dialog->focusWidget();
    if (!initialFocus) initialFocus = firstFocusable(contentLayout);

    auto flags = dialog->windowFlags() | Qt::FramelessWindowHint;
    flags &= ~Qt::WindowMinMaxButtonsHint;
    if (!resizable) flags |= Qt::MSWindowsFixedSizeDialogHint;
    dialog->setWindowFlags(flags);
    dialog->setProperty("dialogFrame", true);
    dialog->setAttribute(Qt::WA_StyledBackground);

    m_titleBar = new TitleBarWidget(dialog, TitleBarWidget::Mode::Dialog);
    m_titleBar->setTitle(dialog->windowTitle());
    m_windowFrame = new WindowFrameController(dialog, m_titleBar);
    connect(dialog, &QWidget::windowTitleChanged, m_titleBar, &TitleBarWidget::setTitle);
    // close(), rather than reject()/hide(), preserves each original system-X
    // route, including ignored close events and deferred done()/finished().
    connect(m_titleBar, &TitleBarWidget::closeRequested, dialog, &QWidget::close);
    connect(m_titleBar, &TitleBarWidget::systemMoveRequested,
            m_windowFrame, &WindowFrameController::startSystemMove);
    connect(m_titleBar, &TitleBarWidget::systemMenuRequested,
            m_windowFrame, &WindowFrameController::showSystemMenu);

    auto *scroll = new QScrollArea(dialog);
    scroll->setObjectName(QStringLiteral("dialogFrameScrollArea"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    scroll->setMinimumSize(0, 0);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#dialogFrameScrollArea { background: transparent; border: none; }"));
    m_content = new QWidget;
    m_content->setObjectName(QStringLiteral("dialogFrameContent"));
    m_content->setAutoFillBackground(false);
    m_content->setLayout(contentLayout);
    // Widgets originally created without a parent only become dialog controls
    // when the layout adopts them. Choose focus after that reparenting as well.
    if (!initialFocus) initialFocus = firstFocusable(contentLayout);
    const QSize bodyMinimum = originalMinimum.expandedTo(contentLayout->totalMinimumSize());
    m_content->setMinimumSize(bodyMinimum);
    scroll->setWidget(m_content);
    scroll->viewport()->setAutoFillBackground(false);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(Border, Border, Border, Border);
    layout->setSpacing(0);
    layout->addWidget(m_titleBar);
    layout->addWidget(scroll, 1);

    QSize available(800, 800);
    if (const auto *screen = dialog->parentWidget() ? dialog->parentWidget()->screen()
                                                  : QGuiApplication::primaryScreen())
        available = screen->availableGeometry().size();
    const QSize screenLimit = (available - QSize(32, 32)).expandedTo(QSize(1, 1));
    QSize maximum = withDecoration(originalMaximum);
    const QSize minimum = withDecoration(bodyMinimum).boundedTo(screenLimit).boundedTo(maximum);
    // A genuinely fixed axis stays fixed even if the initial screen cannot
    // fit the old size. Its original content is still reachable by scrolling.
    if (originalMinimum.width() == originalMaximum.width()) maximum.setWidth(minimum.width());
    if (originalMinimum.height() == originalMaximum.height()) maximum.setHeight(minimum.height());
    dialog->setMaximumSize(maximum);
    dialog->setMinimumSize(minimum);
    const QSize desired = withDecoration(explicitlyResized ? originalSize : contentLayout->totalSizeHint());
    dialog->resize(desired.boundedTo(screenLimit).boundedTo(maximum).expandedTo(minimum));
    if (initialFocus) initialFocus->setFocus(Qt::OtherFocusReason);
    dialog->installEventFilter(this);
}

bool DialogFrame::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Show && m_firstShow) {
        m_firstShow = false;
        auto *dialog = qobject_cast<QDialog *>(watched);
        // A caller may apply the application theme after construction. Measure
        // again after polishing so an auto-sized form doesn't compress editors
        // to the height of its earlier, unstyled size hint.
        m_content->ensurePolished();
        auto *bodyLayout = m_content->layout();
        bodyLayout->invalidate();
        const QSize bodyMinimum = m_originalMinimum.expandedTo(bodyLayout->totalMinimumSize());
        m_content->setMinimumSize(bodyMinimum);
        QSize available(800, 800);
        if (const auto *screen = dialog->screen()) available = screen->availableGeometry().size();
        const QSize limit = (available - QSize(32, 32)).expandedTo(QSize(1, 1));
        const QSize minimum = withDecoration(bodyMinimum).boundedTo(limit).boundedTo(dialog->maximumSize());
        dialog->setMinimumSize(minimum);
        if (m_autoSize) dialog->resize(withDecoration(bodyLayout->totalSizeHint())
            .boundedTo(limit).boundedTo(dialog->maximumSize()).expandedTo(minimum));
    }
    return QObject::eventFilter(watched, event);
}

bool DialogFrame::handleNativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    return m_windowFrame && m_windowFrame->handleNativeEvent(eventType, message, result);
}

void DialogFrame::setCloseEnabled(bool enabled)
{
    m_titleBar->closeButton()->setEnabled(enabled);
}
} // namespace asmr
