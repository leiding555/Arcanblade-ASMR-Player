#include "ui/PlayerMessageBox.h"

#include "ui/TitleBarWidget.h"
#include "ui/WindowFrameController.h"

#include <QResizeEvent>
#include <QShowEvent>
#include <QPushButton>

namespace asmr {

PlayerMessageBox::PlayerMessageBox(QWidget *parent)
    : QMessageBox(parent)
{
    initializeFrame();
}

PlayerMessageBox::PlayerMessageBox(Icon icon, const QString &title, const QString &text,
                                 StandardButtons buttons, QWidget *parent, Qt::WindowFlags flags)
    : QMessageBox(icon, title, text, buttons, parent, flags)
{
    initializeFrame();
}

void PlayerMessageBox::initializeFrame()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    setOption(QMessageBox::Option::DontUseNativeDialog);
#endif
    setWindowFlags((windowFlags() | Qt::FramelessWindowHint)
                   & ~(Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint));
    setProperty("dialogFrame", true);
    setAttribute(Qt::WA_StyledBackground);
    // QMessageBox rebuilds its own grid when text, details or buttons change.
    // Widget margins survive those rebuilds without moving any Qt-owned items.
    setContentsMargins(0, TitleBarWidget::DialogHeight, 0, 0);
    m_titleBar = new TitleBarWidget(this, TitleBarWidget::Mode::Dialog);
    m_titleBar->setTitle(windowTitle());
    m_windowFrame = new WindowFrameController(this, m_titleBar);
    connect(this, &QWidget::windowTitleChanged, m_titleBar, &TitleBarWidget::setTitle);
    connect(m_titleBar, &TitleBarWidget::closeRequested, this, &QWidget::close);
    connect(m_titleBar, &TitleBarWidget::systemMoveRequested,
            m_windowFrame, &WindowFrameController::startSystemMove);
    connect(m_titleBar, &TitleBarWidget::systemMenuRequested,
            m_windowFrame, &WindowFrameController::showSystemMenu);
    positionTitleBar();
}

void PlayerMessageBox::positionTitleBar()
{
    if (!m_titleBar)
        return;
    m_titleBar->setGeometry(0, 0, width(), TitleBarWidget::DialogHeight);
    m_titleBar->raise();
}

bool PlayerMessageBox::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_windowFrame && m_windowFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QMessageBox::nativeEvent(eventType, message, result);
}

void PlayerMessageBox::resizeEvent(QResizeEvent *event)
{
    QMessageBox::resizeEvent(event);
    positionTitleBar();
}

void PlayerMessageBox::showEvent(QShowEvent *event)
{
    if (auto *close = button(QMessageBox::Close)) close->setText(QStringLiteral("关闭"));
    QMessageBox::showEvent(event);
    positionTitleBar();
}

QMessageBox::StandardButton PlayerMessageBox::warning(QWidget *parent, const QString &title,
                                                    const QString &text, StandardButtons buttons,
                                                    StandardButton defaultButton)
{
    PlayerMessageBox dialog(Warning, title, text, buttons, parent);
    if (defaultButton != NoButton)
        dialog.setDefaultButton(defaultButton);
    return static_cast<StandardButton>(dialog.exec());
}

} // namespace asmr
