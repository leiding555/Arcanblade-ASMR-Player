#pragma once

#include <QObject>
#include <QSize>

class QDialog;
class QLayout;
class QWidget;

namespace asmr {
class TitleBarWidget;
class WindowFrameController;

// Window decoration only. The QDialog keeps its original closeEvent/done,
// button results, modality and any asynchronous work owned by its caller.
class DialogFrame final : public QObject {
    Q_OBJECT
public:
    // contentLayout must not already be installed on a widget. Its controls
    // keep their layout; a scroll host makes them reachable on small screens.
    DialogFrame(QDialog *dialog, QLayout *contentLayout, bool resizable = true);
    [[nodiscard]] TitleBarWidget *titleBar() const { return m_titleBar; }
    [[nodiscard]] QWidget *contentWidget() const { return m_content; }
    bool handleNativeEvent(const QByteArray &eventType, void *message, qintptr *result);
    void setCloseEnabled(bool enabled);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    TitleBarWidget *m_titleBar = nullptr;
    WindowFrameController *m_windowFrame = nullptr;
    QWidget *m_content = nullptr;
    QSize m_originalMinimum;
    bool m_autoSize = false;
    bool m_firstShow = true;
};
} // namespace asmr
