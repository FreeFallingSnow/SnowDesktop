#include "app/app.h"
#include "app/dock/persistent_dock_host.h"
#include "dock/dock_window_preview.h"
#include "layout/floating_popup_rules.h"
#include "system/status_bar/status_bar.h"
#include "ui/input/private_pointer.h"
#include "ui/input/touch_input.h"

#include <array>

namespace
{
namespace touch = snowdesktop::touch_input;

bool IsVertical(DockPosition position)
{
    return position == DockPosition::Left || position == DockPosition::Right;
}

int InwardTravel(DockPosition position, POINT start, POINT end)
{
    switch (position)
    {
    case DockPosition::Left: return end.x - start.x;
    case DockPosition::Right: return start.x - end.x;
    case DockPosition::Top: return end.y - start.y;
    case DockPosition::Bottom: return start.y - end.y;
    }
    return 0;
}

int EdgeDistance(DockPosition position, const RECT& bounds, POINT point)
{
    return InwardTravel(position, {
        position == DockPosition::Right ? bounds.right - 1 : bounds.left,
        position == DockPosition::Bottom ? bounds.bottom - 1 : bounds.top}, point);
}

bool IsOutwardSwipe(DockPosition position, const touch::Contact& contact, POINT end, int threshold)
{
    const int alongBar = IsVertical(position) ? std::abs(end.y - contact.start.y) :
        std::abs(end.x - contact.start.x);
    const int inward = InwardTravel(position, contact.start, end);
    return inward >= threshold && inward >= alongBar;
}

bool IsNewContact(UINT message, WPARAM pointer)
{
    return message == WM_POINTERDOWN && IS_POINTER_PRIMARY_WPARAM(pointer) &&
        touch::IsTouchPointer(pointer);
}

struct DockTouch
{
    HWND window = nullptr;
    touch::Contact contact;
    bool dragging = false;
    bool slideOut = false;
};

struct EdgeTouch
{
    HWND window = nullptr;
    touch::Contact contact;
    HMONITOR monitor = nullptr;
    bool dock = false;
    bool statusBar = false;
};

struct TouchEdgeWindow
{
    DesktopApp* app = nullptr;
    HMONITOR monitor = nullptr;
    DockPosition edge = DockPosition::Bottom;
    UINT dpi = 96;
    touch::Contact contact;
};

constexpr wchar_t kTouchEdgeWindowClass[] = L"SnowDesktop.TouchEdge";

RECT TouchEdgeBounds(RECT bounds, DockPosition side, UINT dpi)
{
    constexpr int bandDip = 8;
    const int band = touch::Dips(bandDip, dpi);
    const int corner = touch::Dips(20, dpi);
    if (IsVertical(side))
    {
        bounds.top += corner;
        bounds.bottom -= corner;
        if (side == DockPosition::Left) bounds.right = bounds.left + band;
        else bounds.left = bounds.right - band;
    }
    else
    {
        bounds.left += corner;
        bounds.right -= corner;
        if (side == DockPosition::Top) bounds.bottom = bounds.top + band;
        else bounds.top = bounds.bottom - band;
    }
    return bounds;
}
}

void DesktopApp::ObserveDesktopTouch(HWND window, UINT message, WPARAM pointer, LPARAM position)
{
    if (snowdesktop::private_pointer::Active()) return;
    const bool touched = touch::Observe(message, pointer);
    static thread_local bool visualTouchMode = false;
    if (visualTouchMode != touch::touchMode)
    {
        visualTouchMode = touch::touchMode;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (!touched || touch::routingTouch) return;
    if (message != WM_POINTERDOWN && message != WM_LBUTTONDOWN &&
        message != WM_LBUTTONUP && message != WM_POINTERCAPTURECHANGED) return;

    OnMouseLeave();
    UpdateLargeIconHover();

    if (message != WM_POINTERDOWN || !IS_POINTER_PRIMARY_WPARAM(pointer)) return;
    const POINT screen = touch::ScreenPoint(position);
    if (dockWindowPreview_ && dockWindowPreview_->TouchOpened() &&
        !dockWindowPreview_->ContainsInteractionPoint(screen) &&
        !PtInRect(&dockWindowPreviewAnchorScreen_, screen)) HideDockWindowPreview();
    if (window != floatingPopupHwnd_ && floatingPopupHwnd_ && IsWindowVisible(floatingPopupHwnd_))
        HandleFloatingPopupExternalPointerDown(floatingPopupMouseHookGeneration_,
            snowdesktop::floating_popup_rules::PackScreenPoint(screen));
}

bool DesktopApp::HandleDockTouch(HWND window, UINT message, WPARAM pointer, LPARAM position)
{
    if (!touch::IsPointerMessage(message) || snowdesktop::private_pointer::Active()) return false;
    static thread_local DockTouch gesture;
    if (gesture.window && !IsWindow(gesture.window)) gesture = {};
    const POINT screen = touch::ScreenPoint(position);
    if (IsNewContact(message, pointer))
    {
        if (gesture.contact.Active() || !dockSettings_.showWindowPreviews || !hwnd_) return false;
        POINT desktop = screen;
        if (!ScreenToClient(hwnd_, &desktop)) return false;
        DockWindowPreviewTarget target;
        if (!ResolveDockWindowPreviewTarget(desktop, target)) return false;
        gesture = {};
        gesture.window = window;
        gesture.contact.Begin(pointer, screen);
        return true;
    }
    if (gesture.window != window || !gesture.contact.Owns(pointer)) return false;
    if (message == WM_POINTERCAPTURECHANGED || touch::IsCanceled(pointer))
    {
        const bool dragging = gesture.dragging;
        gesture = {};
        if (dragging) SendMessageW(window, WM_CANCELMODE, 0, 0);
        return true;
    }
    if (message == WM_POINTERUPDATE || message == WM_POINTERUP)
    {
        auto& contact = gesture.contact;
        const int slop = touch::Dips(12, GetDpiForWindow(window));
        if (!gesture.dragging && !gesture.slideOut && contact.Moved(screen, slop))
        {
            gesture.slideOut = IsOutwardSwipe(dockSettings_.position, contact, screen, slop);
            gesture.dragging = !gesture.slideOut;
            if (gesture.dragging) touch::RouteMouse(window, WM_LBUTTONDOWN, contact.start, MK_LBUTTON);
        }
        if (message == WM_POINTERUPDATE)
        {
            contact.last = screen;
            if (gesture.dragging) touch::RouteMouse(window, WM_MOUSEMOVE, screen, MK_LBUTTON);
            return true;
        }

        const DockTouch released = gesture;
        gesture = {};
        if (released.dragging) touch::RouteMouse(window, WM_LBUTTONUP, screen);
        else if (released.slideOut)
        {
            POINT origin = released.contact.start;
            if (ScreenToClient(hwnd_, &origin)) (void)TryOpenDockWindowPicker(origin, true);
        }
        else if (released.contact.Held())
        {
            touch::RouteMouse(window, WM_RBUTTONDOWN, screen, MK_RBUTTON);
            touch::RouteMouse(window, WM_RBUTTONUP, screen);
        }
        else
        {
            touch::RouteMouse(window, WM_LBUTTONDOWN, released.contact.start, MK_LBUTTON);
            touch::RouteMouse(window, WM_LBUTTONUP, screen);
        }
    }
    return true;
}

bool DesktopApp::HandleDesktopTouchEdge(HWND window, UINT message, WPARAM pointer, LPARAM position)
{
    if (!touch::IsPointerMessage(message) || snowdesktop::private_pointer::Active()) return false;
    static thread_local EdgeTouch gesture;
    if (gesture.window && !IsWindow(gesture.window)) gesture = {};
    const POINT screen = touch::ScreenPoint(position);
    if (IsNewContact(message, pointer))
    {
        if (gesture.contact.Active() || desktopPassthroughActive_) return false;
        const HMONITOR monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONULL);
        MONITORINFO info{sizeof(info)};
        if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
        UINT dpi = GetDpiForWindow(window);
        if (UINT x = 0, y = 0; SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) dpi = x;
        const int band = touch::Dips(8, dpi);
        const bool dock = generalSettings_.dockEnabled && dockSettings_.showOnlyWhenSummoned &&
            EdgeDistance(dockSettings_.position, info.rcMonitor, screen) < band &&
            !FullscreenBlocksDockReveal(DockRevealSource::Gesture, monitor);
        const bool status = statusBar_ && statusBar_->CanRevealFromTouch(monitor) &&
            EdgeDistance(generalSettings_.statusBar.position, info.rcMonitor, screen) < band;
        if (!dock && !status) return false;
        gesture = {};
        gesture.window = window;
        gesture.monitor = monitor;
        gesture.dock = dock;
        gesture.statusBar = status;
        gesture.contact.Begin(pointer, screen);
        return true;
    }
    if (gesture.window != window || !gesture.contact.Owns(pointer)) return false;
    if (message == WM_POINTERCAPTURECHANGED || touch::IsCanceled(pointer))
    {
        gesture = {};
        return true;
    }
    if (message == WM_POINTERUP)
    {
        const auto released = gesture;
        gesture = {};
        const int travel = touch::Dips(24, GetDpiForWindow(window));
        if (released.dock && IsOutwardSwipe(dockSettings_.position, released.contact, screen, travel))
            ShowFloatingDock(released.monitor, DockRevealSource::Gesture);
        if (released.statusBar && statusBar_ &&
            IsOutwardSwipe(generalSettings_.statusBar.position, released.contact, screen, travel))
            statusBar_->RevealFromTouch(released.monitor);
    }
    return true;
}

LRESULT CALLBACK DesktopApp::TouchEdgeWndProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* edge = reinterpret_cast<TouchEdgeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!edge) return DefWindowProcW(window, message, wp, lp);
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_POINTERACTIVATE) return PA_NOACTIVATE;
    if (touch::IsPointerMessage(message) && edge->app->HandleDesktopTouchEdge(window, message, wp, lp))
        return 0;
    if (message == WM_CANCELMODE || message == WM_POINTERCAPTURECHANGED)
        edge->contact.Reset();
    if (message == WM_NCDESTROY)
    {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete edge;
    }
    if (message != WM_TOUCH) return DefWindowProcW(window, message, wp, lp);

    std::array<TOUCHINPUT, 32> input{};
    const UINT count = LOWORD(wp);
    const auto handle = reinterpret_cast<HTOUCHINPUT>(lp);
    const bool received = count <= input.size() && GetTouchInputInfo(
        handle, count, input.data(), sizeof(TOUCHINPUT));
    CloseTouchInputHandle(handle);
    if (!received || snowdesktop::private_pointer::Active()) return 0;

    for (UINT index = 0; index < count; ++index)
    {
        const auto& sample = input[index];
        const POINT point{TOUCH_COORD_TO_PIXEL(sample.x), TOUCH_COORD_TO_PIXEL(sample.y)};
        if ((sample.dwFlags & (TOUCHEVENTF_PRIMARY | TOUCHEVENTF_DOWN)) ==
            (TOUCHEVENTF_PRIMARY | TOUCHEVENTF_DOWN) &&
            (sample.dwFlags & (TOUCHEVENTF_PEN | TOUCHEVENTF_PALM)) == 0)
        {
            touch::touchMode = true;
            edge->contact.id = sample.dwID;
            edge->contact.start = edge->contact.last = point;
            edge->contact.started = GetTickCount64();
            edge->app->OnMouseLeave();
        }
        if (edge->contact.id != sample.dwID || (sample.dwFlags & TOUCHEVENTF_UP) == 0) continue;
        const auto released = edge->contact;
        edge->contact.Reset();
        if (!released.Active() || !IsOutwardSwipe(edge->edge, released, point, touch::Dips(24, edge->dpi)))
            continue;
        auto& app = *edge->app;
        if (app.generalSettings_.dockEnabled && app.dockSettings_.showOnlyWhenSummoned &&
            app.dockSettings_.position == edge->edge)
            app.ShowFloatingDock(edge->monitor, DockRevealSource::Gesture);
        if (app.statusBar_ && app.generalSettings_.statusBar.position == edge->edge)
            app.statusBar_->RevealFromTouch(edge->monitor);
    }
    return 0;
}

void DesktopApp::SyncTouchEdgeWindows()
{
    const bool enabled = hwnd_ && !exitRequested_ && !desktopPassthroughActive_ &&
        ((GetSystemMetrics(SM_DIGITIZER) & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH)) ||
            generalSettings_.touchSimulationEnabled);
    const bool dock = enabled && generalSettings_.dockEnabled && dockSettings_.showOnlyWhenSummoned;
    const bool status = enabled && statusBar_ && generalSettings_.statusBar.showOnlyWhenSummoned;
    if (!dock && !status) { DestroyTouchEdgeWindows(); return; }

    struct Target { HMONITOR monitor; DockPosition edge; RECT bounds; UINT dpi; };
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM owner) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(owner)->push_back(monitor);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    std::vector<Target> targets;
    for (const auto monitor : monitors)
    {
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) continue;
        UINT dpi = 96;
        if (UINT x = 0, y = 0; SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) dpi = x;
        const bool dockEligible = dock && !FullscreenBlocksDockReveal(DockRevealSource::Gesture, monitor) &&
            (!floatingDockVisible_ || !floatingDockHost_ || floatingDockHost_->monitor != monitor);
        const auto add = [&](DockPosition edge) {
            const RECT bounds = TouchEdgeBounds(info.rcMonitor, edge, dpi);
            if (!IsRectEmpty(&bounds)) targets.push_back({monitor, edge, bounds, dpi});
        };
        if (dockEligible) add(dockSettings_.position);
        if (status && statusBar_->CanRevealFromTouch(monitor) &&
            (!dockEligible || generalSettings_.statusBar.position != dockSettings_.position))
            add(generalSettings_.statusBar.position);
    }

    std::erase_if(touchEdgeWindows_, [&](HWND window) {
        const auto* edge = reinterpret_cast<TouchEdgeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (edge && std::ranges::any_of(targets, [&](const auto& target) {
            return target.monitor == edge->monitor && target.edge == edge->edge;
        })) return false;
        DestroyWindow(window);
        return true;
    });
    if (targets.empty()) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = TouchEdgeWndProc;
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kTouchEdgeWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    for (const auto& target : targets)
    {
        const auto it = std::ranges::find_if(touchEdgeWindows_, [&](HWND window) {
            const auto* edge = reinterpret_cast<TouchEdgeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            return edge && target.monitor == edge->monitor && target.edge == edge->edge;
        });
        HWND window = it == touchEdgeWindows_.end() ? nullptr : *it;
        const auto& bounds = target.bounds;
        if (!window)
        {
            window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kTouchEdgeWindowClass, nullptr, WS_POPUP, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance_, nullptr);
            if (!window) continue;
            if (!RegisterTouchWindow(window, 0) || !SetLayeredWindowAttributes(window, 0, 1, LWA_ALPHA))
            { DestroyWindow(window); continue; }
            auto edge = std::make_unique<TouchEdgeWindow>(TouchEdgeWindow{this, target.monitor, target.edge, target.dpi, {}});
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(edge.release()));
            touchEdgeWindows_.push_back(window);
        }
        auto* edge = reinterpret_cast<TouchEdgeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        edge->dpi = target.dpi;
        RECT placed{};
        GetWindowRect(window, &placed);
        if (!IsWindowVisible(window) || !EqualRect(&placed, &bounds))
            SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
}

void DesktopApp::DestroyTouchEdgeWindows()
{
    for (const HWND window : touchEdgeWindows_) DestroyWindow(window);
    touchEdgeWindows_.clear();
}
