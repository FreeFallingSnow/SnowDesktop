#include "tray_hover_popup.h"
#include <algorithm>
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

void HoverPopup::Watch(HWND receiver, HWND target, DWORD process, RECT iconBounds, POINT popupAnchor)
{
    Reset();
    DWORD actualProcess = 0;
    if (!receiver || !process || !GetWindowThreadProcessId(target, &actualProcess) ||
        actualProcess != process || process == GetCurrentProcessId()) return;

    receiver_ = receiver;
    target_ = target;
    process_ = process;
    started_ = GetTickCount();
    const LONG distance = MulDiv(96, static_cast<int>(GetDpiForWindow(receiver)), USER_DEFAULT_SCREEN_DPI);
    const RECT anchorBounds{popupAnchor.x, popupAnchor.y, popupAnchor.x + 1, popupAnchor.y + 1};
    UnionRect(&vicinity_, &iconBounds, &anchorBounds);
    InflateRect(&vicinity_, distance, distance);
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
    return std::any_of(shown_.begin(), shown_.end(), [this](HWND popup) { return IsNearAnchor(popup); });
}

bool HoverPopup::Covers(HWND window) const
{
    // A mouse-leave message can arrive before the out-of-context show event.
    return hook_ && window && IsNearAnchor(GetAncestor(window, GA_ROOT));
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

    auto slot = std::find(self.shown_.begin(), self.shown_.end(), window);
    if (event == EVENT_OBJECT_SHOW)
    {
        // The window can become visible off-screen, then move beside the icon.
        if (!self.IsCandidate(window)) return;
        if (slot == self.shown_.end())
            slot = std::find_if(self.shown_.begin(), self.shown_.end(),
                [&](HWND popup) { return !self.IsCandidate(popup); });
        if (slot == self.shown_.end()) return;
        *slot = window;
    }
    else if (event == EVENT_OBJECT_HIDE)
    {
        if (slot == self.shown_.end()) return;
        *slot = nullptr;
    }
    else if (slot == self.shown_.end()) return;

    const UINT changed = HoverPopupChangedMessage();
    if (changed) PostMessageW(self.receiver_, changed, 0, 0);
}
}
