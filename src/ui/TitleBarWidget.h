#pragma once

#include "ui/ApplicationBrand.h"

#include <QWidget>

class QLabel;
class QEvent;
class QMouseEvent;
class QToolButton;

namespace asmr {

// Presentation and window-control intentions only. The owner decides how to
// perform native moves, state changes and its asynchronous close protocol.
class TitleBarWidget final : public QWidget {
    Q_OBJECT
public:
    enum class Mode { MainWindow, Dialog };
    static constexpr int Height = 48;
    static constexpr int DialogHeight = 40;

    explicit TitleBarWidget(QWidget *parent = nullptr, Mode mode = Mode::MainWindow);

    [[nodiscard]] Mode mode() const;
    [[nodiscard]] QToolButton *minimizeButton() const;
    [[nodiscard]] QToolButton *maximizeButton() const;
    [[nodiscard]] QToolButton *closeButton() const;
    [[nodiscard]] bool isDragPosition(const QPoint &localPosition) const;

    void setTitle(const QString &title);
    // Hiding the title and decoration leaves native hit testing and controls intact.
    void setTitleVisible(bool visible);
    // The owner supplies its sidebar bounds mapped into this bar. Only the
    // horizontal span is used; window controls keep their native-sized slots.
    void setBrandAlignmentRect(const QRect &rect);
    // Horizontal center of the sidebar import icon, mapped into this bar.
    void setBrandIconCenterX(int logicalX);
    void setMaximized(bool maximized);
    void setWindowActive(bool active);
    void setClosing(bool closing);
    void setNativeMaximizeHovered(bool hovered);
    void setNativeMaximizePressed(bool pressed);

signals:
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();
    void systemMoveRequested();
    void systemMenuRequested(const QPoint &globalPosition);

protected:
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void updateApplicationIcon();
    void updateBrandGeometry();

    QLabel *m_title = nullptr;
    QLabel *m_applicationIcon = nullptr;
    QToolButton *m_minimize = nullptr;
    QToolButton *m_maximize = nullptr;
    QToolButton *m_close = nullptr;
    Mode m_mode = Mode::MainWindow;
    QString m_titleText = applicationDisplayName();
    QRect m_brandAlignmentRect;
    int m_brandIconCenterX = -1;
    bool m_maximized = false;
    bool m_closing = false;
};

} // namespace asmr
