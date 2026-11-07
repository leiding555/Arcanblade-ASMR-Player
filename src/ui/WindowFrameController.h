#pragma once

#include <QByteArray>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <Qt>

class QWidget;

namespace asmr {

class TitleBarWidget;

// All coordinates in these helpers use the same (native physical pixel) space.
namespace frameGeometry {
Qt::Edges resizeEdges(const QRect &frame, const QPoint &position,
                      int borderWidth, bool maximized);
QRect maximizeBounds(const QRect &monitor, const QRect &workArea);
QSize physicalMinimumSize(const QSize &logicalMinimum, qreal devicePixelRatio);
}

// The window remains the owner of close and playback lifecycle decisions.
// This adapter only supplies the operating system's non-client window behavior.
class WindowFrameController final : public QObject
{
    Q_OBJECT

public:
    explicit WindowFrameController(QWidget *window, TitleBarWidget *titleBar);

    bool handleNativeEvent(const QByteArray &eventType, void *message, qintptr *result);
    bool usesNativeFrame() const;
    void startSystemMove();
    void showSystemMenu(const QPoint &globalLogicalPosition);
    void syncWindowState();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool isDialogMode() const;
    bool allowsResize() const;
    void configureNativeFrame();
    void clearNativeButtonState();
    bool isMaximizePosition(const QPoint &nativeScreenPosition) const;
    void finishNativeMaximizePress(const QPoint &nativeScreenPosition);
    void showNativeSystemMenu(const QPoint &nativeScreenPosition);

    QPointer<QWidget> m_window;
    QPointer<TitleBarWidget> m_titleBar;
    quintptr m_configuredWinId = 0;
    bool m_configuring = false;
    bool m_maximizePressed = false;
};

} // namespace asmr
