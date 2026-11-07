#include "ui/ButtonFocusController.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFileDialog>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QWidget>

namespace asmr {
namespace {
QWidget *focusScope(QWidget *widget)
{
    QWidget *scope = nullptr;
    for (auto *ancestor = widget; ancestor; ancestor = ancestor->parentWidget()) {
        // Also exclude Qt's fallback file chooser and all its internal buttons.
        if (qobject_cast<QFileDialog *>(ancestor)) return nullptr;
        if (!scope && ancestor->property("buttonFocusScope").toBool()) scope = ancestor;
    }
    return scope;
}

QAbstractButton *managedButton(QWidget *widget)
{
    if (!widget || !focusScope(widget)) return nullptr;
    if (auto *button = qobject_cast<QPushButton *>(widget)) return button;
    return qobject_cast<QToolButton *>(widget);
}
} // namespace

ButtonFocusController::ButtonFocusController(QObject *parent) : QObject(parent)
{
    setObjectName(QStringLiteral("asmrButtonFocusController"));
    qApp->installEventFilter(this);
}

ButtonFocusController *ButtonFocusController::instance()
{
    auto *controller = qApp->findChild<ButtonFocusController *>(
        QStringLiteral("asmrButtonFocusController"), Qt::FindDirectChildrenOnly);
    return controller ? controller : new ButtonFocusController(qApp);
}

void ButtonFocusController::install(QWidget *root)
{
    Q_ASSERT(root && qApp);
    root->setProperty("buttonFocusScope", true);
    instance();
}

void ButtonFocusController::notifyMouseInteraction(QWidget *scope)
{
    if (scope && focusScope(scope)) instance()->mouseInteraction();
}

void ButtonFocusController::setKeyboardFocus(QAbstractButton *button, bool visible)
{
    if (!button || (button->property("buttonKeyboardFocus").isValid()
        && button->property("buttonKeyboardFocus").toBool() == visible)) return;
    button->setProperty("buttonKeyboardFocus", visible);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
}

void ButtonFocusController::mouseInteraction()
{
    m_keyboardInput = false;
    // The remembered button may temporarily lose focus to its popup menu.
    setKeyboardFocus(m_lastButton, false);
    setKeyboardFocus(managedButton(QApplication::focusWidget()), false);
}

bool ButtonFocusController::eventFilter(QObject *watched, QEvent *event)
{
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick
        && type != QEvent::KeyPress && type != QEvent::FocusIn) return false;
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || !focusScope(widget)) return false;

    if (event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonDblClick) {
        mouseInteraction();
        if (auto *button = managedButton(widget)) {
            setKeyboardFocus(button, false);
            m_lastButton = button;
        }
    } else if (event->type() == QEvent::KeyPress) {
        // Escape only dismisses a popup. It should return the opener's focus
        // presentation rather than turn a mouse-opened menu into a Tab stop.
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            for (auto *ancestor = widget; ancestor; ancestor = ancestor->parentWidget())
                if (qobject_cast<QMenu *>(ancestor)) return false;
        }
        m_keyboardInput = true;
        if (auto *button = managedButton(QApplication::focusWidget())) {
            setKeyboardFocus(button, true);
            m_lastButton = button;
        }
    } else if (event->type() == QEvent::FocusIn) {
        if (auto *button = managedButton(widget)) {
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
                || reason == Qt::ShortcutFocusReason) {
                m_keyboardInput = true;
                setKeyboardFocus(button, true);
            } else if (reason == Qt::MouseFocusReason) {
                mouseInteraction();
                setKeyboardFocus(button, false);
            } else {
                setKeyboardFocus(button, m_keyboardInput);
            }
            // Use the latest input even when a different dialog was focused in
            // between. Mouse dismissal must not revive an older keyboard ring.
            m_lastButton = button;
        }
    }
    return false; // Qt retains all focus, menu, default-button and click handling.
}

} // namespace asmr
