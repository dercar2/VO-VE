#include "window_frame.hpp"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEnterEvent>
#include <QHoverEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QVariant>
#include <QWidget>
#include <QWindow>
#include <QtMath>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace vove::ui {
namespace {

bool interactive_widget(const QWidget *widget) {
    if (widget->focusPolicy() != Qt::NoFocus || widget->testAttribute(Qt::WA_InputMethodEnabled) ||
        qobject_cast<const QAbstractButton *>(widget) || qobject_cast<const QLineEdit *>(widget) ||
        qobject_cast<const QComboBox *>(widget) || qobject_cast<const QAbstractSlider *>(widget) ||
        qobject_cast<const QAbstractSpinBox *>(widget) ||
        qobject_cast<const QAbstractScrollArea *>(widget)) {
        return true;
    }
    const auto *label = qobject_cast<const QLabel *>(widget);
    return label && label->textInteractionFlags() != Qt::NoTextInteraction;
}

#ifdef Q_OS_WIN
constexpr LONG_PTR frame_style =
    WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
// Undocumented themed-caption paint messages, not declared by the Windows SDK.
constexpr UINT wm_nc_uah_draw_caption = 0x00AE;
constexpr UINT wm_nc_uah_draw_frame = 0x00AF;

QPoint logical_position(HWND hwnd, LPARAM coordinates, const QWidget *window,
                        const bool screen_coordinates = true) {
    POINT point{GET_X_LPARAM(coordinates), GET_Y_LPARAM(coordinates)};
    if (screen_coordinates) {
        ScreenToClient(hwnd, &point);
    }
    // Scale client-relative distances, never virtual-desktop origins. Qt's DPR also
    // includes QT_SCALE_FACTOR, unlike GetDpiForWindow()/96 alone.
    const auto scale = window->devicePixelRatioF();
    return {qFloor(point.x / scale), qFloor(point.y / scale)};
}

QSize native_border(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    const int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    return {GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + padding,
            GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + padding};
}

LRESULT native_resize_hit(Qt::Edges edges) {
    if (edges.testFlag(Qt::TopEdge)) {
        return edges.testFlag(Qt::LeftEdge)    ? HTTOPLEFT
               : edges.testFlag(Qt::RightEdge) ? HTTOPRIGHT
                                               : HTTOP;
    }
    if (edges.testFlag(Qt::BottomEdge)) {
        return edges.testFlag(Qt::LeftEdge)    ? HTBOTTOMLEFT
               : edges.testFlag(Qt::RightEdge) ? HTBOTTOMRIGHT
                                               : HTBOTTOM;
    }
    return edges.testFlag(Qt::LeftEdge) ? HTLEFT : HTRIGHT;
}
#endif

} // namespace

Qt::Edges window_frame_resize_edges(const QRect &bounds, const QPoint &position,
                                    const QSize &border) noexcept {
    if (!bounds.contains(position)) {
        return {};
    }
    Qt::Edges edges;
    if (position.x() - bounds.left() < border.width()) {
        edges |= Qt::LeftEdge;
    } else if (bounds.right() - position.x() < border.width()) {
        edges |= Qt::RightEdge;
    }
    if (position.y() - bounds.top() < border.height()) {
        edges |= Qt::TopEdge;
    } else if (bounds.bottom() - position.y() < border.height()) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

bool window_frame_is_drag_area(QWidget *window, QWidget *title_bar, const QPoint &window_position) {
    if (!window || !title_bar || title_bar->window() != window || !title_bar->isVisibleTo(window) ||
        !window->rect().contains(window_position) ||
        !title_bar->rect().contains(title_bar->mapFrom(window, window_position))) {
        return false;
    }
    auto *hit = window->childAt(window_position);
    if (!hit) {
        hit = window;
    }
    if (hit != title_bar && !title_bar->isAncestorOf(hit)) {
        return false;
    }
    bool allowed = hit == title_bar || hit->metaObject() == &QWidget::staticMetaObject;
    for (auto *widget = hit; widget; widget = widget->parentWidget()) {
        if (interactive_widget(widget)) {
            return false;
        }
        allowed = allowed || widget->property("windowDragArea").toBool();
        if (widget == title_bar) {
            return allowed;
        }
    }
    return false;
}

WindowFrame::WindowFrame(QWidget *window, QWidget *title_bar, QAbstractButton *maximize_button)
    : QObject(window), window_(window), title_bar_(title_bar), maximize_button_(maximize_button) {
    Q_ASSERT(window && window->isWindow());
    if (!window || !window->isWindow()) {
        return;
    }
#ifdef Q_OS_WIN
    native_enabled_ = QGuiApplication::platformName() == QStringLiteral("windows");
#endif
    // Application filtering also covers children added later, but every event is scoped
    // to this top-level widget; dialogs and popup menus retain their normal behavior.
    qApp->installEventFilter(this);
    if (native_enabled_) {
        qApp->installNativeEventFilter(this);
    }
    had_hover_ = window->testAttribute(Qt::WA_Hover);
    if (!native_enabled_) {
        window->setAttribute(Qt::WA_Hover);
        window->setWindowFlag(Qt::FramelessWindowHint);
    }
    update_native_window();
}

WindowFrame::~WindowFrame() {
    if (qApp) {
        qApp->removeEventFilter(this);
        qApp->removeNativeEventFilter(this);
    }
    cancel_maximize_press();
    set_maximize_hover(false);
    update_resize_cursor({});
    if (window_ && !native_enabled_) {
        window_->setAttribute(Qt::WA_Hover, had_hover_);
    }
#ifdef Q_OS_WIN
    const auto hwnd = reinterpret_cast<HWND>(native_window_);
    if (window_ && hwnd && window_->internalWinId() == native_window_ && IsWindow(hwnd)) {
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          (style & ~frame_style) | static_cast<LONG_PTR>(original_frame_style_));
        const DWMNCRENDERINGPOLICY policy = DWMNCRP_USEWINDOWSTYLE;
        DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#endif
}

Qt::Edges WindowFrame::resize_edges(const QPoint &position, const QSize &border) const {
    if (!window_ || window_->isMaximized() || window_->isFullScreen()) {
        return {};
    }
    auto edges = window_frame_resize_edges(window_->rect(), position, border);
    if (window_->minimumWidth() >= window_->maximumWidth()) {
        edges &= ~(Qt::LeftEdge | Qt::RightEdge);
    }
    if (window_->minimumHeight() >= window_->maximumHeight()) {
        edges &= ~(Qt::TopEdge | Qt::BottomEdge);
    }
    return edges;
}

bool WindowFrame::maximize_area(const QPoint &position) const {
    if (!window_ || window_->isFullScreen() || !maximize_button_ ||
        maximize_button_->window() != window_ || !maximize_button_->isEnabled() ||
        !maximize_button_->isVisibleTo(window_)) {
        return false;
    }
#ifdef Q_OS_WIN
    // Qt blocks modal owners at the HWND level without disabling their QWidgets.
    if (native_enabled_ && native_window_ &&
        !IsWindowEnabled(reinterpret_cast<HWND>(native_window_))) {
        return false;
    }
#endif
    const auto *hit = window_->childAt(position);
    return hit && (hit == maximize_button_ || maximize_button_->isAncestorOf(hit));
}

void WindowFrame::update_resize_cursor(Qt::Edges edges) {
    if (!window_) {
        return;
    }
    if (!edges) {
        if (cursor_changed_) {
            if (had_cursor_) {
                window_->setCursor(previous_cursor_);
            } else {
                window_->unsetCursor();
            }
            cursor_changed_ = false;
        }
        return;
    }
    if (!cursor_changed_) {
        had_cursor_ = window_->testAttribute(Qt::WA_SetCursor);
        previous_cursor_ = window_->cursor();
        cursor_changed_ = true;
    }
    const bool horizontal = edges.testAnyFlags(Qt::LeftEdge | Qt::RightEdge);
    const bool vertical = edges.testAnyFlags(Qt::TopEdge | Qt::BottomEdge);
    const auto shape =
        horizontal && vertical
            ? (edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge) ? Qt::SizeFDiagCursor
                                                                           : Qt::SizeBDiagCursor)
            : (horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
    window_->setCursor(shape);
}

void WindowFrame::set_maximize_hover(bool hovered) {
    if (maximize_hovered_ == hovered) {
        return;
    }
    maximize_hovered_ = hovered;
    if (maximize_button_) {
        maximize_button_->setAttribute(Qt::WA_UnderMouse, hovered);
        if (hovered) {
            const auto local = maximize_button_->rect().center();
            QEnterEvent event(local, maximize_button_->mapTo(window_, local),
                              maximize_button_->mapToGlobal(local));
            QCoreApplication::sendEvent(maximize_button_, &event);
        } else {
            QEvent event(QEvent::Leave);
            QCoreApplication::sendEvent(maximize_button_, &event);
        }
        maximize_button_->update();
    }
}

void WindowFrame::cancel_maximize_press() {
    const bool was_pressed = maximize_pressed_;
    maximize_pressed_ = false;
    if (was_pressed && maximize_button_) {
        maximize_button_->setDown(false);
    }
#ifdef Q_OS_WIN
    if (was_pressed && native_window_ && GetCapture() == reinterpret_cast<HWND>(native_window_)) {
        ReleaseCapture();
    }
#else
    Q_UNUSED(was_pressed);
#endif
}

void WindowFrame::update_native_window() {
#ifdef Q_OS_WIN
    if (!native_enabled_ || !window_) {
        return;
    }
    const auto id = static_cast<quintptr>(window_->internalWinId());
    if (id == native_window_) {
        return;
    }
    cancel_maximize_press();
    native_window_ = id;
    if (!id) {
        return;
    }
    const auto hwnd = reinterpret_cast<HWND>(id);
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    original_frame_style_ = static_cast<quintptr>(style & frame_style);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | frame_style);
    // Keep the style semantics for snapping, but never ask DWM to paint caption buttons.
    const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    // Keep Qt's normal Windows state transitions; its frameless flag emulates
    // maximization instead. WM_NCCALCSIZE removes the native caption visually.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

bool WindowFrame::eventFilter(QObject *watched, QEvent *event) {
    if (!window_) {
        return false;
    }
    if (watched == window_ || watched == window_->windowHandle()) {
        switch (event->type()) {
        case QEvent::WinIdChange:
        case QEvent::Show:
            update_native_window();
            break;
        case QEvent::Hide:
        case QEvent::WindowDeactivate:
        case QEvent::WindowStateChange:
        case QEvent::WindowBlocked:
            cancel_maximize_press();
            set_maximize_hover(false);
            update_resize_cursor({});
            break;
        case QEvent::HoverMove:
            if (!native_enabled_) {
                update_resize_cursor(resize_edges(
                    static_cast<QHoverEvent *>(event)->position().toPoint(), QSize(6, 6)));
            }
            break;
        case QEvent::HoverLeave:
            update_resize_cursor({});
            break;
        default:
            break;
        }
    }
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick) {
        return false;
    }
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != window_ || !window_->isEnabled()) {
        return false;
    }
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton || window_->isFullScreen()) {
        return false;
    }
    const auto position = widget->mapTo(window_, mouse->position().toPoint());
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (window_frame_is_drag_area(window_, title_bar_, position)) {
            window_->isMaximized() ? window_->showNormal() : window_->showMaximized();
            return true;
        }
    } else if (!native_enabled_ && window_->windowHandle()) {
        const auto edges = resize_edges(position, QSize(6, 6));
        if (edges) {
            return window_->windowHandle()->startSystemResize(edges);
        }
        if (window_frame_is_drag_area(window_, title_bar_, position)) {
            return window_->windowHandle()->startSystemMove();
        }
    }
    return false;
}

bool WindowFrame::nativeEventFilter(const QByteArray &event_type, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    if (!native_enabled_ || !window_ || !native_window_ || !message ||
        (event_type != "windows_generic_MSG" && event_type != "windows_dispatcher_MSG")) {
        return false;
    }
    // Qt's Win32 dispatcher passes null for queued input, unlike synchronous messages.
    qintptr unused_result{};
    if (!result) {
        result = &unused_result;
    }
    auto *msg = static_cast<MSG *>(message);
    const auto hwnd = reinterpret_cast<HWND>(native_window_);
    if (msg->hwnd != hwnd) {
        return false;
    }
    switch (msg->message) {
    case WM_STYLECHANGING:
        if (static_cast<int>(msg->wParam) == GWL_STYLE && msg->lParam) {
            reinterpret_cast<STYLESTRUCT *>(msg->lParam)->styleNew |=
                static_cast<DWORD>(frame_style);
        }
        return false;
    case WM_NCCALCSIZE:
        if (msg->lParam && IsZoomed(hwnd) && !window_->isFullScreen()) {
            auto *rect = msg->wParam ? &reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam)->rgrc[0]
                                     : reinterpret_cast<RECT *>(msg->lParam);
            MONITORINFO monitor{sizeof(MONITORINFO), {}, {}, 0};
            if (GetMonitorInfoW(MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST), &monitor)) {
                *rect = monitor.rcWork;
            }
        }
        *result = 0;
        return true;
    case WM_GETMINMAXINFO: {
        if (!msg->lParam) {
            return false;
        }
        auto *info = reinterpret_cast<MINMAXINFO *>(msg->lParam);
        MONITORINFO monitor{sizeof(MONITORINFO), {}, {}, 0};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto &area = window_->isFullScreen() ? monitor.rcMonitor : monitor.rcWork;
            info->ptMaxPosition = {area.left - monitor.rcMonitor.left,
                                   area.top - monitor.rcMonitor.top};
            info->ptMaxSize = {area.right - area.left, area.bottom - area.top};
        }
        const auto scale = window_->devicePixelRatioF();
        info->ptMinTrackSize.x = qCeil(window_->minimumWidth() * scale);
        info->ptMinTrackSize.y = qCeil(window_->minimumHeight() * scale);
        if (window_->maximumWidth() < QWIDGETSIZE_MAX) {
            info->ptMaxTrackSize.x = qFloor(window_->maximumWidth() * scale);
        }
        if (window_->maximumHeight() < QWIDGETSIZE_MAX) {
            info->ptMaxTrackSize.y = qFloor(window_->maximumHeight() * scale);
        }
        *result = 0;
        return true;
    }
    case WM_NCHITTEST: {
        const auto position = logical_position(hwnd, msg->lParam, window_);
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        auto edges = window_frame_resize_edges(QRect(0, 0, client.right, client.bottom),
                                               QPoint(point.x, point.y), native_border(hwnd));
        if (IsZoomed(hwnd) || window_->isMaximized() || window_->isFullScreen()) {
            edges = {};
        }
        if (window_->minimumWidth() >= window_->maximumWidth()) {
            edges &= ~(Qt::LeftEdge | Qt::RightEdge);
        }
        if (window_->minimumHeight() >= window_->maximumHeight()) {
            edges &= ~(Qt::TopEdge | Qt::BottomEdge);
        }
        *result =
            edges                     ? native_resize_hit(edges)
            : maximize_area(position) ? HTMAXBUTTON
            : !window_->isFullScreen() && window_frame_is_drag_area(window_, title_bar_, position)
                ? HTCAPTION
                : HTCLIENT;
        return true;
    }
    case WM_NCPAINT:
    case wm_nc_uah_draw_caption:
    case wm_nc_uah_draw_frame:
        // Disabling DWM rendering does not stop themed/classic caption painting.
        // These paths can paint over our client area during hover or modal activation.
        *result = 0;
        return true;
    case WM_NCACTIVATE:
        // TRUE allows activation/deactivation; WM_ACTIVATE still reaches Qt. With
        // no native frame to paint, DefWindowProc can only introduce caption artifacts.
        // Minimized windows retain the documented default activation handling.
        *result = IsIconic(hwnd) ? DefWindowProcW(hwnd, msg->message, msg->wParam, -1) : TRUE;
        return true;
    case WM_NCMOUSEMOVE: {
        const bool inside = maximize_area(logical_position(hwnd, msg->lParam, window_));
        set_maximize_hover(inside);
        if (inside) {
            TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            TrackMouseEvent(&track);
        }
        // Let Windows handle non-client hover, including the Windows 11 Snap menu.
        return false;
    }
    case WM_NCMOUSELEAVE:
        set_maximize_hover(false);
        return false;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (msg->wParam == HTMAXBUTTON &&
            maximize_area(logical_position(hwnd, msg->lParam, window_))) {
            maximize_pressed_ = true;
            maximize_button_->setDown(true);
            SetCapture(hwnd);
            *result = 0;
            return true;
        }
        return false;
    case WM_MOUSEMOVE:
        if (maximize_pressed_) {
            const bool inside = maximize_area(logical_position(hwnd, msg->lParam, window_, false));
            if (maximize_button_) {
                maximize_button_->setDown(inside);
            }
            set_maximize_hover(inside);
            *result = 0;
            return true;
        }
        return false;
    case WM_NCLBUTTONUP:
    case WM_LBUTTONUP:
        if (maximize_pressed_) {
            const bool activate = maximize_area(
                logical_position(hwnd, msg->lParam, window_, msg->message == WM_NCLBUTTONUP));
            const QPointer<QAbstractButton> button = maximize_button_;
            cancel_maximize_press();
            *result = 0;
            // Consume both native messages. Only the parent's regular clicked handler
            // changes state; DefWindowProc must not also perform SC_MAXIMIZE/RESTORE.
            if (activate && button) {
                button->click();
            }
            return true;
        }
        return false;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        cancel_maximize_press();
        return false;
    case WM_NCDESTROY:
        cancel_maximize_press();
        native_window_ = 0;
        return false;
    default:
        return false;
    }
#else
    Q_UNUSED(event_type);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}

} // namespace vove::ui
