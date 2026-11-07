#pragma once

#include <QObject>
#include <QPointer>

class QAbstractButton;
class QWidget;

namespace asmr {

// Tracks the source of button focus without changing Qt's focus or activation
// semantics. The shared theme paints one solid border for keyboard focus only.
class ButtonFocusController final : public QObject {
    Q_OBJECT
public:
    static void install(QWidget *root);
    // Native non-client clicks (e.g. Windows Snap's maximize button) do not
    // generate a QWidget mouse press, but still switch to mouse presentation.
    static void notifyMouseInteraction(QWidget *scope);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit ButtonFocusController(QObject *parent);
    static ButtonFocusController *instance();
    void mouseInteraction();
    void setKeyboardFocus(QAbstractButton *button, bool visible);

    QPointer<QAbstractButton> m_lastButton;
    bool m_keyboardInput = false;
};

} // namespace asmr
