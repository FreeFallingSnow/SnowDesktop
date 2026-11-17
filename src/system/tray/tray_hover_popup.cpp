#include "tray_hover_popup.h"
#include <algorithm>
#include <dwmapi.h>
#include <unordered_map>

namespace snowdesktop::tray
{
namespace
{
thread_local std::unordered_map<HWINEVENTHOOK, HoverPopup*> observers;
}

UINT HoverPopupChangedMessage()
{
    static const UINT message = RegisterWindowMessageW(L"SnowDesktop.Tray.HoverPopupChanged");
    return message;
}

HoverPopup::~HoverPopup()
{
    Reset();
}

void HoverPopup::Watch(HWND receiver, HWND target, DWORD process, RECT iconBounds, RECT row, bool preferAbove, bool firstShowArmed)
{
    Reset();
    DWORD actualProcess = 0;
    if (!receiver || !process || !GetWindowThreadProcessId(target, &actualProcess) ||
        actualProcess != process || process == GetCurrentProcessId()) return;

    receiver_ = receiver;
    target_ = target;
    process_ = process;
    started_ = GetTickCount();
    const int dpi = static_cast<int>(GetDpiForWindow(receiver));
    const LONG distance = MulDiv(96, dpi, USER_DEFAULT_SCREEN_DPI);
    vicinity_ = iconBounds;
    InflateRect(&vicinity_, distance, distance);
    row_ = row;
    preferAbove_ = preferAbove;
    firstShowArmed_ = firstShowArmed;
    gap_ = MulDiv(6, dpi, USER_DEFAULT_SCREEN_DPI);
    MONITORINFO monitor{sizeof(monitor)};
    workArea_ = GetMonitorInfoW(MonitorFromRect(&iconBounds, MONITOR_DEFAULTTONEAREST), &monitor)
        ? monitor.rcWork : RECT{};
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnEvent,
        process, 0, WINEVENT_OUTOFCONTEXT);
    if (hook_) observers.emplace(hook_, this);
}

void HoverPopup::Reset()
{
    if (hook_)
    {
        observers.erase(hook_);
        UnhookWinEvent(hook_);
        hook_ = nullptr;
    }
    shown_ = {};
}

bool HoverPopup::IsCandidate(HWND window) const
{
    DWORD process = 0;
    if (!window || window == target_ || !IsWindowVisible(window) ||
        !GetWindowThreadProcessId(window, &process) || process != process_) return false;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & (WS_CHILD | WS_SYSMENU | WS_THICKFRAME)) ||
        (extended & (WS_EX_APPWINDOW | WS_EX_TRANSPARENT)))
        return false;
    // QQ's card uses a frameless Chromium widget with caption style bits.
    const auto overlay = WS_EX_TOPMOST | WS_EX_LAYERED;
    return ((style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION) ||
        (extended & overlay) == overlay;
}

bool HoverPopup::IsNearAnchor(HWND window) const
{
    if (!IsCandidate(window)) return false;
    RECT bounds{}, intersection{};
    return GetWindowRect(window, &bounds) && IntersectRect(&intersection, &bounds, &vicinity_);
}

bool HoverPopup::Visible() const
{
    return std::any_of(shown_.begin(), shown_.end(), [this](const Popup& popup) {
        return popup.positionRequested ? IsCandidate(popup.window) : IsNearAnchor(popup.window);
    });
}

bool HoverPopup::Covers(HWND window) const
{
    // A mouse-leave message can arrive before the out-of-context show event.
    if (!hook_ || !window) return false;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (IsNearAnchor(root)) return true;
    return std::any_of(shown_.begin(), shown_.end(), [&](const Popup& popup) {
        return popup.window == root && popup.positionRequested && IsCandidate(root);
    });
}

void HoverPopup::AvoidRow(Popup& popup, const RECT& bounds) const
{
    RECT overlap{};
    if (firstShowArmed_ || popup.positionRequested || !IntersectRect(&overlap, &bounds, &row_)) return;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(popup.window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return;
    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || width > workArea_.right - workArea_.left ||
        height > workArea_.bottom - workArea_.top) return;

    const LONG x = std::clamp(bounds.left, workArea_.left, workArea_.right - width);
    const LONG y = std::clamp(bounds.top, workArea_.top, workArea_.bottom - height);
    const POINT above{x, row_.top - gap_ - height};
    const POINT below{x, row_.bottom + gap_};
    const POINT left{row_.left - gap_ - width, y};
    const POINT right{row_.right + gap_, y};
    const std::array positions{preferAbove_ ? above : below, preferAbove_ ? below : above, left, right};
    const auto position = std::find_if(positions.begin(), positions.end(), [&](POINT point) {
        return point.x >= workArea_.left && point.x <= workArea_.right - width &&
            point.y >= workArea_.top && point.y <= workArea_.bottom - height;
    });
    if (position == positions.end() || !IsCandidate(popup.window)) return;
    // One attempt per show. Apps may reject the request or lay out again.
    popup.positionRequested = true;
    SetWindowPos(popup.window, nullptr, position->x, position->y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void CALLBACK HoverPopup::OnEvent(HWINEVENTHOOK hook, DWORD event, HWND window,
    LONG object, LONG child, DWORD, DWORD time)
{
    if (!window || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_LOCATIONCHANGE) return;
    const auto observer = observers.find(hook);
    if (observer == observers.end()) return;
    auto& self = *observer->second;
    if (window == self.target_ || static_cast<LONG>(time - self.started_) < 0) return;

    auto slot = std::find_if(self.shown_.begin(), self.shown_.end(),
        [window](const Popup& popup) { return popup.window == window; });
    if (event == EVENT_OBJECT_SHOW)
    {
        // The window can become visible off-screen, then move beside the icon.
        if (!self.IsCandidate(window)) return;
        if (slot == self.shown_.end())
        {
            slot = std::find_if(self.shown_.begin(), self.shown_.end(),
                [&](const Popup& popup) { return !self.IsCandidate(popup.window); });
            if (slot == self.shown_.end()) return;
            *slot = {window};
        }
    }
    else if (event == EVENT_OBJECT_HIDE)
    {
        if (slot == self.shown_.end()) return;
        *slot = {};
    }
    else if (slot == self.shown_.end()) return;

    const UINT changed = HoverPopupChangedMessage();
    if (changed) PostMessageW(self.receiver_, changed, 0, 0);
    RECT bounds{};
    if (slot->window && GetWindowRect(window, &bounds)) self.AvoidRow(*slot, bounds);
}
}
