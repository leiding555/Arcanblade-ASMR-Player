#pragma once

#include <QMessageBox>

namespace asmr {

class TitleBarWidget;
class WindowFrameController;

// Retain QMessageBox's button results, details layout and close handling. The
// title bar occupies widget contents margins, outside its Qt-owned layout.
class PlayerMessageBox final : public QMessageBox {
    Q_OBJECT
public:
    explicit PlayerMessageBox(QWidget *parent = nullptr);
    PlayerMessageBox(Icon icon, const QString &title, const QString &text,
                     StandardButtons buttons = NoButton, QWidget *parent = nullptr,
                     Qt::WindowFlags flags = Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);

    static StandardButton warning(QWidget *parent, const QString &title, const QString &text,
                                  StandardButtons buttons = Ok,
                                  StandardButton defaultButton = NoButton);

    [[nodiscard]] TitleBarWidget *titleBar() const { return m_titleBar; }

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void initializeFrame();
    void positionTitleBar();

    TitleBarWidget *m_titleBar = nullptr;
    WindowFrameController *m_windowFrame = nullptr;
};

} // namespace asmr
