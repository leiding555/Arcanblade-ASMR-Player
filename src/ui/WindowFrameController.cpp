#include "ui/WindowFrameController.h"

#include "ui/ButtonFocusController.h"
#include "ui/TitleBarWidget.h"

#include <QEvent>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScopedValueRollback>
#include <QToolButton>
#include <QWidget>
#include <QWindow>
#include <QtMath>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace asmr {

namespace frameGeometry {

Qt::Edges resizeEdges(const QRect &frame, const QPoint &position,
                      int borderWidth, bool maximized)
{
    if (maximized || borderWidth <= 0 || !frame.isValid() || !frame.contains(position))
        return {};

    // Keep opposite edges mutually exclusive even for unusually small windows.
    const int horizontalBorder = std::min(borderWidth, (frame.width() + 1) / 2);
    const int verticalBorder = std::min(borderWidth, (frame.height() + 1) / 2);
    Qt::Edges edges;
    if (position.x() < frame.left() + horizontalBorder)
        edges |= Qt::LeftEdge;
    else if (position.x() > frame.right() - horizontalBorder)
        edges |= Qt::RightEdge;
    if (position.y() < frame.top() + verticalBorder)
        edges |= Qt::TopEdge;
    else if (position.y() > frame.bottom() - verticalBorder)
        edges |= Qt::BottomEdge;
    return edges;
}

QRect maximizeBounds(const QRect &monitor, const QRect &workArea)
{
    if (!monitor.isValid())
        return {};
    const QRect available = monitor.intersected(workArea);
    const QRect bounded = available.isValid() ? available : monitor;
    return QRect(bounded.topLeft() - monitor.topLeft(), bounded.size());
}

QSize physicalMinimumSize(const QSize &logicalMinimum, qreal devicePixelRatio)
{
    const qreal ratio = std::isfinite(devicePixelRatio) && devicePixelRatio > 0
        ? devicePixelRatio : 1.0;
    return QSize(qCeil(std::max(0, logicalMinimum.width()) * ratio),
                 qCeil(std::max(0, logicalMinimum.height()) * ratio));
}

} // namespace frameGeometry

#ifdef Q_OS_WIN
namespace {

HWND nativeHandle(const QWidget *window)
{
    return window ? reinterpret_cast<HWND>(window->internalWinId()) : nullptr;
}

UINT windowDpi(HWND handle)
{
    using GetDpiForWindowFunction = UINT(WINAPI *)(HWND);
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFunction>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = getDpiForWindow ? getDpiForWindow(handle) : 96;
    return dpi == 0 ? 96 : dpi;
}

int resizeBorder(HWND handle)
{
    using GetSystemMetricsForDpiFunction = int(WINAPI *)(int, UINT);
    static const auto getSystemMetricsForDpi = reinterpret_cast<GetSystemMetricsForDpiFunction>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    const UINT dpi = windowDpi(handle);
    const auto metric = [dpi](int index) {
        return getSystemMetricsForDpi ? getSystemMetricsForDpi(index, dpi)
                                     : GetSystemMetrics(index);
    };
    return std::max(1, metric(SM_CXSIZEFRAME) + metric(SM_CXPADDEDBORDER));
}

QRect nativeRect(const RECT &rect)
{
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

QPoint nativeScreenPoint(LPARAM position)
{
    // LOWORD/HIWORD without signed extraction breaks monitors left/above primary.
    return QPoint(GET_X_LPARAM(position), GET_Y_LPARAM(position));
}

QPoint clientLogicalPoint(const QWidget *window, const QPoint &nativePosition)
{
    POINT client = {nativePosition.x(), nativePosition.y()};
    ScreenToClient(nativeHandle(window), &client);
    const qreal ratio = window->devicePixelRatioF();
    return QPoint(qFloor(client.x / ratio), qFloor(client.y / ratio));
}

int resizeHitTest(Qt::Edges edges)
{
    if (edges == (Qt::TopEdge | Qt::LeftEdge))
        return HTTOPLEFT;
    if (edges == (Qt::TopEdge | Qt::RightEdge))
        return HTTOPRIGHT;
    if (edges == (Qt::BottomEdge | Qt::LeftEdge))
        return HTBOTTOMLEFT;
    if (edges == (Qt::BottomEdge | Qt::RightEdge))
        return HTBOTTOMRIGHT;
    if (edges == Qt::LeftEdge)
        return HTLEFT;
    if (edges == Qt::RightEdge)
        return HTRIGHT;
    if (edges == Qt::TopEdge)
        return HTTOP;
    if (edges == Qt::BottomEdge)
        return HTBOTTOM;
    return HTCLIENT;
}

// Runtime resolution keeps Windows 10 compatibility and avoids an extra library
// dependency. Unsupported Windows 11 attributes fail harmlessly on Windows 10.
void applyDwmAppearance(HWND handle)
{
    struct DwmFunctions {
        HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        using ExtendFrame = HRESULT(WINAPI *)(HWND, const MARGINS *);
        SetAttribute setAttribute = module ? reinterpret_cast<SetAttribute>(
            GetProcAddress(module, "DwmSetWindowAttribute")) : nullptr;
        ExtendFrame extendFrame = module ? reinterpret_cast<ExtendFrame>(
            GetProcAddress(module, "DwmExtendFrameIntoClientArea")) : nullptr;
        ~DwmFunctions() { if (module) FreeLibrary(module); }
    };
    static const DwmFunctions dwm;
    if (dwm.setAttribute) {
        constexpr DWORD darkModeAttribute = 20;
        constexpr DWORD cornerPreferenceAttribute = 33;
        const BOOL darkMode = TRUE;
        const DWORD roundCorners = 2;
        dwm.setAttribute(handle, darkModeAttribute, &darkMode, sizeof(darkMode));
        dwm.setAttribute(handle, cornerPreferenceAttribute, &roundCorners, sizeof(roundCorners));
    }
    if (dwm.extendFrame) {
        const MARGINS shadowMargins = {1, 1, 1, 1};
        dwm.extendFrame(handle, &shadowMargins);
    }
}

} // namespace
#endif

WindowFrameController::WindowFrameController(QWidget *window, TitleBarWidget *titleBar)
    : QObject(window), m_window(window), m_titleBar(titleBar)
{
    Q_ASSERT(m_window);
    Q_ASSERT(m_titleBar);
    m_window->installEventFilter(this);
    syncWindowState();
    configureNativeFrame();
}

bool WindowFrameController::usesNativeFrame() const
{
#ifdef Q_OS_WIN
    return m_window && QGuiApplication::platformName() == QStringLiteral("windows");
#else
    return false;
#endif
}

bool WindowFrameController::isDialogMode() const
{
    return m_titleBar && m_titleBar->mode() == TitleBarWidget::Mode::Dialog;
}

bool WindowFrameController::allowsResize() const
{
    return !isDialogMode() || (m_window
        && !m_window->windowFlags().testFlag(Qt::MSWindowsFixedSizeDialogHint));
}

void WindowFrameController::configureNativeFrame()
{
#ifdef Q_OS_WIN
    if (!usesNativeFrame() || m_configuring)
        return;
    const HWND handle = nativeHandle(m_window);
    if (!handle) {
        m_configuredWinId = 0;
        return;
    }
    if (m_configuredWinId == reinterpret_cast<quintptr>(handle))
        return;
    QScopedValueRollback<bool> configuring(m_configuring, true);
    const LONG_PTR style = GetWindowLongPtrW(handle, GWL_STYLE);
    LONG_PTR nativeStyle = isDialogMode()
        ? (style | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU) & ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
        : style | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    if (!allowsResize()) nativeStyle &= ~WS_THICKFRAME;
    SetWindowLongPtrW(handle, GWL_STYLE, nativeStyle);
    m_configuredWinId = reinterpret_cast<quintptr>(handle);
    applyDwmAppearance(handle);
    SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

void WindowFrameController::clearNativeButtonState()
{
#ifdef Q_OS_WIN
    const bool ownedCapture = m_maximizePressed && usesNativeFrame()
        && GetCapture() == nativeHandle(m_window);
#endif
    // ReleaseCapture can synchronously send WM_CAPTURECHANGED. Clear ownership
    // first so cancellation cannot recurse or release another widget's capture.
    m_maximizePressed = false;
    if (m_titleBar) {
        m_titleBar->setNativeMaximizeHovered(false);
        m_titleBar->setNativeMaximizePressed(false);
    }
#ifdef Q_OS_WIN
    if (ownedCapture)
        ReleaseCapture();
#endif
}

void WindowFrameController::syncWindowState()
{
    if (!m_window || !m_titleBar)
        return;
    m_titleBar->setMaximized(m_window->isMaximized());
    m_titleBar->setWindowActive(m_window->isActiveWindow());
    clearNativeButtonState();
}

void WindowFrameController::startSystemMove()
{
    if (m_window && m_window->windowHandle())
        m_window->windowHandle()->startSystemMove();
}

void WindowFrameController::showSystemMenu(const QPoint &globalLogicalPosition)
{
#ifdef Q_OS_WIN
    if (isDialogMode() || !usesNativeFrame() || !nativeHandle(m_window))
        return;
    const QPoint logicalClient = m_window->mapFromGlobal(globalLogicalPosition);
    const qreal ratio = m_window->devicePixelRatioF();
    POINT nativeClient = {qRound(logicalClient.x() * ratio), qRound(logicalClient.y() * ratio)};
    ClientToScreen(nativeHandle(m_window), &nativeClient);
    showNativeSystemMenu(QPoint(nativeClient.x, nativeClient.y));
#else
    Q_UNUSED(globalLogicalPosition);
#endif
}

void WindowFrameController::showNativeSystemMenu(const QPoint &nativeScreenPosition)
{
#ifdef Q_OS_WIN
    if (isDialogMode())
        return;
    const HWND handle = nativeHandle(m_window);
    const HMENU menu = GetSystemMenu(handle, FALSE);
    if (!menu)
        return;
    const bool maximized = IsZoomed(handle);
    const auto enable = [menu](UINT command, bool enabled) {
        EnableMenuItem(menu, command, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    enable(SC_RESTORE, maximized || IsIconic(handle));
    enable(SC_MOVE, !maximized);
    enable(SC_SIZE, !maximized);
    enable(SC_MINIMIZE, !IsIconic(handle));
    enable(SC_MAXIMIZE, !maximized);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                       nativeScreenPosition.x(), nativeScreenPosition.y(),
                                       0, handle, nullptr);
    if (command != 0)
        PostMessageW(handle, WM_SYSCOMMAND, command, 0);
#else
    Q_UNUSED(nativeScreenPosition);
#endif
}

bool WindowFrameController::isMaximizePosition(const QPoint &nativeScreenPosition) const
{
#ifdef Q_OS_WIN
    if (isDialogMode() || !m_window || !m_titleBar || !m_titleBar->isVisible()
        || !m_titleBar->maximizeButton()->isVisible()
        || !m_titleBar->maximizeButton()->isEnabled())
        return false;
    const QPoint local = m_titleBar->maximizeButton()->mapFrom(
        m_window, clientLogicalPoint(m_window, nativeScreenPosition));
    return m_titleBar->maximizeButton()->rect().contains(local);
#else
    Q_UNUSED(nativeScreenPosition);
    return false;
#endif
}

void WindowFrameController::finishNativeMaximizePress(const QPoint &nativeScreenPosition)
{
#ifdef Q_OS_WIN
    const bool activate = m_maximizePressed && isMaximizePosition(nativeScreenPosition)
        && m_titleBar->maximizeButton()->isEnabled();
    clearNativeButtonState();
    // A native HTMAXBUTTON press never reaches Qt's client mouse handlers. Send
    // exactly one normal button action, using the same guarded window-state path
    // as keyboard activation, after the native message has returned.
    if (activate)
        QMetaObject::invokeMethod(m_titleBar->maximizeButton(), &QToolButton::click,
                                  Qt::QueuedConnection);
#else
    Q_UNUSED(nativeScreenPosition);
#endif
}

bool WindowFrameController::handleNativeEvent(const QByteArray &eventType,
                                             void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (!usesNativeFrame() || !m_titleBar || eventType != "windows_generic_MSG" || !message || !result)
        return false;
    const auto *nativeMessage = static_cast<MSG *>(message);
    const HWND handle = nativeHandle(m_window);
    if (!handle || nativeMessage->hwnd != handle)
        return false;
    switch (nativeMessage->message) {
    case WM_SYSCOMMAND:
        if (isDialogMode()) {
            const WPARAM command = nativeMessage->wParam & 0xFFF0;
            if (command == SC_MINIMIZE || command == SC_MAXIMIZE
                || command == SC_RESTORE || command == SC_KEYMENU) {
                *result = 0;
                return true;
            }
        }
        break; // In particular, SC_CLOSE still uses the owner's close protocol.
    case WM_NCCALCSIZE:
        // Keep native resizing/system commands while making the whole window a
        // Qt-painted client area. WM_GETMINMAXINFO supplies taskbar-safe bounds.
        *result = 0;
        return true;
    case WM_GETMINMAXINFO: {
        auto *limits = reinterpret_cast<MINMAXINFO *>(nativeMessage->lParam);
        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        if (!limits || !GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
            return false;
        const QRect bounds = frameGeometry::maximizeBounds(nativeRect(monitor.rcMonitor),
                                                           nativeRect(monitor.rcWork));
        limits->ptMaxPosition = {bounds.x(), bounds.y()};
        limits->ptMaxSize = {bounds.width(), bounds.height()};
        const QSize minimum = frameGeometry::physicalMinimumSize(m_window->minimumSize(),
                                                                 m_window->devicePixelRatioF());
        limits->ptMinTrackSize = {minimum.width(), minimum.height()};
        // QWidget's unconstrained sentinel must not become a physical-pixel
        // maximum. Otherwise keep explicit application constraints intact.
        const QSize maximum = m_window->maximumSize();
        const qreal ratio = m_window->devicePixelRatioF();
        if (maximum.width() < QWIDGETSIZE_MAX)
            limits->ptMaxTrackSize.x = qFloor(maximum.width() * ratio);
        if (maximum.height() < QWIDGETSIZE_MAX)
            limits->ptMaxTrackSize.y = qFloor(maximum.height() * ratio);
        *result = 0;
        return true;
    }
    case WM_NCHITTEST: {
        RECT rect = {};
        if (!GetWindowRect(handle, &rect))
            return false;
        const QPoint position = nativeScreenPoint(nativeMessage->lParam);
        const Qt::Edges edges = allowsResize()
            ? frameGeometry::resizeEdges(nativeRect(rect), position,
                resizeBorder(handle), IsZoomed(handle) || m_window->isMaximized())
            : Qt::Edges{};
        const int resizeHit = resizeHitTest(edges);
        if (resizeHit != HTCLIENT) {
            *result = resizeHit;
            return true;
        }
        if (isMaximizePosition(position)) {
            // Windows 11 uses this hit result to offer its Snap Layout menu.
            *result = HTMAXBUTTON;
            return true;
        }
        const QPoint titlePosition = m_titleBar->mapFrom(
            m_window, clientLogicalPoint(m_window, position));
        *result = m_titleBar->isVisible() && m_titleBar->isDragPosition(titlePosition)
            ? HTCAPTION : HTCLIENT;
        return true;
    }
    case WM_SETCURSOR: {
        LPCWSTR cursor = nullptr;
        switch (LOWORD(nativeMessage->lParam)) {
        case HTLEFT: case HTRIGHT: cursor = IDC_SIZEWE; break;
        case HTTOP: case HTBOTTOM: cursor = IDC_SIZENS; break;
        case HTTOPLEFT: case HTBOTTOMRIGHT: cursor = IDC_SIZENWSE; break;
        case HTTOPRIGHT: case HTBOTTOMLEFT: cursor = IDC_SIZENESW; break;
        default: break;
        }
        if (cursor) {
            SetCursor(LoadCursorW(nullptr, cursor));
            *result = TRUE;
            return true;
        }
        break;
    }
    case WM_NCMOUSEMOVE: {
        m_titleBar->setNativeMaximizeHovered(nativeMessage->wParam == HTMAXBUTTON);
        TRACKMOUSEEVENT tracking = {sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, handle, 0};
        TrackMouseEvent(&tracking);
        break; // Allow Windows to observe maximize hover for Snap Layouts.
    }
    case WM_NCMOUSELEAVE:
        if (!m_maximizePressed)
            m_titleBar->setNativeMaximizeHovered(false);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        // Native caption/resize presses, including HTMAXBUTTON for Snap, do
        // not pass through Qt's client mouse events.
        ButtonFocusController::notifyMouseInteraction(m_window);
        if (isDialogMode()
            && (nativeMessage->wParam == HTMAXBUTTON || nativeMessage->wParam == HTMINBUTTON
                || (nativeMessage->message == WM_NCLBUTTONDBLCLK
                    && nativeMessage->wParam == HTCAPTION))) {
            *result = 0;
            return true;
        }
        if (nativeMessage->wParam == HTMAXBUTTON) {
            if (m_titleBar->maximizeButton()->isEnabled()) {
                m_maximizePressed = true;
                m_titleBar->setNativeMaximizeHovered(true);
                m_titleBar->setNativeMaximizePressed(true);
                SetCapture(handle);
            }
            *result = 0;
            return true;
        }
        break;
    case WM_MOUSEMOVE:
        if (m_maximizePressed) {
            POINT position = {GET_X_LPARAM(nativeMessage->lParam), GET_Y_LPARAM(nativeMessage->lParam)};
            ClientToScreen(handle, &position);
            const bool inside = isMaximizePosition(QPoint(position.x, position.y));
            m_titleBar->setNativeMaximizeHovered(inside);
            m_titleBar->setNativeMaximizePressed(inside);
            *result = 0;
            return true;
        }
        break;
    case WM_LBUTTONUP:
        if (m_maximizePressed) {
            POINT position = {GET_X_LPARAM(nativeMessage->lParam), GET_Y_LPARAM(nativeMessage->lParam)};
            ClientToScreen(handle, &position);
            finishNativeMaximizePress(QPoint(position.x, position.y));
            *result = 0;
            return true;
        }
        break;
    case WM_NCLBUTTONUP:
        if (isDialogMode()
            && (nativeMessage->wParam == HTMAXBUTTON || nativeMessage->wParam == HTMINBUTTON)) {
            *result = 0;
            return true;
        }
        if (m_maximizePressed || nativeMessage->wParam == HTMAXBUTTON) {
            finishNativeMaximizePress(nativeScreenPoint(nativeMessage->lParam));
            *result = 0;
            return true;
        }
        break;
    case WM_NCRBUTTONDOWN:
        ButtonFocusController::notifyMouseInteraction(m_window);
        if (isDialogMode() && nativeMessage->wParam == HTCAPTION) {
            *result = 0;
            return true;
        }
        break;
    case WM_NCRBUTTONUP:
        if (nativeMessage->wParam == HTCAPTION) {
            showNativeSystemMenu(nativeScreenPoint(nativeMessage->lParam));
            *result = 0;
            return true;
        }
        break;
    case WM_SYSKEYDOWN:
        if (isDialogMode() && nativeMessage->wParam == VK_F4
            && (nativeMessage->lParam & (LPARAM(1) << 29))) {
            // Qt's frameless path can consume Alt+F4 before DefWindowProc sees
            // it. Queue the standard close command, preserving the dialog's
            // rejection/saving guard without closing inside native dispatch.
            PostMessageW(handle, WM_SYSCOMMAND, SC_CLOSE, 0);
            *result = 0;
            return true;
        }
        if (nativeMessage->wParam == VK_SPACE && (nativeMessage->lParam & (LPARAM(1) << 29))) {
            // Qt's frameless platform path may consume Alt+Space before the
            // default window procedure sees it. Keep the standard menu explicit.
            showSystemMenu(m_titleBar->mapToGlobal(QPoint(0, m_titleBar->height())));
            *result = 0;
            return true;
        }
        break;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        clearNativeButtonState();
        break;
    default:
        break;
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

bool WindowFrameController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window) {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::WinIdChange:
            configureNativeFrame();
            break;
        case QEvent::WindowStateChange:
        case QEvent::WindowActivate:
        case QEvent::ActivationChange:
            syncWindowState();
            break;
        case QEvent::Hide:
        case QEvent::WindowDeactivate:
            clearNativeButtonState();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace asmr
