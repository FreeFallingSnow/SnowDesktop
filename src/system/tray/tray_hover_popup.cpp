#include "tray_hover_popup.h"
#include <algorithm>
#include <optional>
#include <unordered_map>

namespace snowdesktop::tray
{
namespace
{
thread_local std::unordered_map<HWINEVENTHOOK, HoverPopup*> observers;

std::optional<POINT> PositionOutsideRow(RECT popup, RECT row, RECT work, LONG gap, bool preferAbove)
{
    RECT overlap{};
    if (!IntersectRect(&overlap, &popup, &row)) return {};
    const LONG width = popup.right - popup.left;
    const LONG height = popup.bottom - popup.top;
    if (width <= 0 || height <= 0 || width > work.right - work.left || height > work.bottom - work.top)
        return {};
    const LONG x = std::clamp(popup.left, work.left, work.right - width);
    const LONG y = std::clamp(popup.top, work.top, work.bottom - height);
    const POINT above{x, row.top - gap - height};
    const POINT below{x, row.bottom + gap};
    const POINT left{row.left - gap - width, y};
    const POINT right{row.right + gap, y};
    const std::array positions{preferAbove ? above : below, preferAbove ? below : above, left, right};
    for (const POINT position : positions)
    {
        if (position.x >= work.left && position.x <= work.right - width &&
            position.y >= work.top && position.y <= work.bottom - height)
            return position;
    }
    return {};
}
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

void HoverPopup::Watch(HWND receiver, HWND target, DWORD process, POINT anchor, RECT row, bool preferAbove)
{
    Reset();
    DWORD actualProcess = 0;
    if (!receiver || !process || !GetWindowThreadProcessId(target, &actualProcess) ||
        actualProcess != process || process == GetCurrentProcessId()) return;

    receiver_ = receiver;
    target_ = target;
    process_ = process;
    started_ = GetTickCount();
    row_ = row;
    preferAbove_ = preferAbove;
    const LONG distance = MulDiv(96, static_cast<int>(GetDpiForWindow(receiver)), USER_DEFAULT_SCREEN_DPI);
    vicinity_ = {anchor.x - distance, anchor.y - distance, anchor.x + distance, anchor.y + distance};
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
    if ((style & (WS_CHILD | WS_SYSMENU | WS_THICKFRAME)) || (extended & WS_EX_APPWINDOW))
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
        return popup.anchored && IsCandidate(popup.window) &&
            (popup.moves || IsNearAnchor(popup.window));
    });
}

bool HoverPopup::Covers(HWND window) const
{
    // A mouse-leave message can arrive before the out-of-context show event.
    if (!hook_ || !window) return false;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (IsNearAnchor(root)) return true;
    return std::any_of(shown_.begin(), shown_.end(), [&](const Popup& popup) {
        return popup.window == root && popup.anchored && popup.moves && IsCandidate(root);
    });
}

void HoverPopup::AvoidRow(Popup& popup) const
{
    if (!popup.anchored || popup.moves >= 2 || IsRectEmpty(&row_) || !IsCandidate(popup.window)) return;
    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    const POINT anchor{vicinity_.left + (vicinity_.right - vicinity_.left) / 2,
        vicinity_.top + (vicinity_.bottom - vicinity_.top) / 2};
    if (!GetWindowRect(popup.window, &bounds) ||
        !GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const LONG gap = MulDiv(6, static_cast<int>(GetDpiForWindow(receiver_)), USER_DEFAULT_SCREEN_DPI);
    const auto position = PositionOutsideRow(bounds, row_, monitor.rcWork, gap, preferAbove_);
    if (!position || !IsCandidate(popup.window)) return;
    // Bound the number of requests when the application insists on its own position.
    ++popup.moves;
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
            if (slot != self.shown_.end()) *slot = {window};
        }
        if (slot == self.shown_.end()) return;
    }
    else if (event == EVENT_OBJECT_HIDE)
    {
        if (slot == self.shown_.end()) return;
        *slot = {};
    }
    else if (slot == self.shown_.end()) return;

    if (slot->window) slot->anchored = slot->anchored || self.IsNearAnchor(window);
    const UINT changed = HoverPopupChangedMessage();
    if (changed) PostMessageW(self.receiver_, changed, 0, 0);
    if (slot->window) self.AvoidRow(*slot);
}
}
