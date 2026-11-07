#include "ui/InputControlPolicy.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QLineEdit>
#include <QResource>
#include <QScopedValueRollback>
#include <QWheelEvent>

// Q_INIT_RESOURCE must be outside the application's namespace.
static void initializeInputResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(input_controls);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace asmr {
namespace {
QWidget *settingControl(QWidget *target)
{
    if (qobject_cast<QComboBox *>(target) || qobject_cast<QAbstractSpinBox *>(target))
        return target;
    // Editors are immediate children. Do not walk through an open combo's
    // popup item view or consume wheel input from any unrelated child widget.
    if (qobject_cast<QLineEdit *>(target)) {
        auto *parent = target->parentWidget();
        if (qobject_cast<QComboBox *>(parent) || qobject_cast<QAbstractSpinBox *>(parent))
            return parent;
    }
    return nullptr;
}

bool belongsToFileDialog(QWidget *widget)
{
    for (auto *ancestor = widget; ancestor; ancestor = ancestor->parentWidget())
        if (qobject_cast<QFileDialog *>(ancestor)) return true;
    return false;
}
} // namespace

void initializeInputControlResources() { initializeInputResources(); }

InputControlPolicy::InputControlPolicy(QObject *parent) : QObject(parent)
{
    setObjectName(QStringLiteral("asmrInputControlPolicy"));
    qApp->installEventFilter(this);
}

void InputControlPolicy::install()
{
    Q_ASSERT(qApp);
    if (!qApp->findChild<InputControlPolicy *>(QStringLiteral("asmrInputControlPolicy"),
                                             Qt::FindDirectChildrenOnly))
        new InputControlPolicy(qApp);
}

bool InputControlPolicy::eventFilter(QObject *watched, QEvent *event)
{
    if (m_forwarding || event->type() != QEvent::Wheel) return false;
    auto *target = qobject_cast<QWidget *>(watched);
    if (!target) return false;
    auto *control = settingControl(target);
    if (!control || belongsToFileDialog(control)) return false;
    if (auto *combo = qobject_cast<QComboBox *>(control); combo && combo->view()->isVisible())
        return false;

    auto *wheel = static_cast<QWheelEvent *>(event);
    QScopedValueRollback forwarding(m_forwarding, true);
    // Sending the wheel to the viewport retains Qt's angle/pixel scrolling,
    // modifiers, phase and nested-scroll behavior. A filter on a selector alone
    // would otherwise either change its value or swallow page scrolling.
    for (auto *ancestor = control->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        auto *area = qobject_cast<QAbstractScrollArea *>(ancestor);
        if (!area || !area->viewport()) continue;
        auto *viewport = area->viewport();
        QWheelEvent forwarded(QPointF(viewport->mapFromGlobal(wheel->globalPosition().toPoint())),
            wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(),
            wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source(), wheel->pointingDevice());
        forwarded.setAccepted(false);
        QApplication::sendEvent(viewport, &forwarded);
        if (forwarded.isAccepted()) break;
    }
    wheel->accept();
    return true;
}
} // namespace asmr
