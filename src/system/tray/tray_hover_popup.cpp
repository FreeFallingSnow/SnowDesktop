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

void HoverPopup::Watch(HWND receiver, HWND target, DWORD process, POINT anchor)
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
    vicinity_ = {anchor.x - distance, anchor.y - distance, anchor.x + distance, anchor.y + distance};
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, OnEvent,
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

bool HoverPopup::IsNearAnchor(HWND window) const
{
    DWORD process = 0;
    if (!window || !IsWindowVisible(window) || !GetWindowThreadProcessId(window, &process) || process != process_)
        return false;
    RECT bounds{}, intersection{};
    return GetWindowRect(window, &bounds) && IntersectRect(&intersection, &bounds, &vicinity_);
}

bool HoverPopup::Visible() const
{
    return std::any_of(shown_.begin(), shown_.end(), [this](HWND window) { return IsNearAnchor(window); });
}

void CALLBACK HoverPopup::OnEvent(HWINEVENTHOOK hook, DWORD event, HWND window,
    LONG object, LONG child, DWORD, DWORD time)
{
    if (!window || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    const auto observer = observers.find(hook);
    if (observer == observers.end()) return;
    auto& self = *observer->second;
    if (window == self.target_ || static_cast<LONG>(time - self.started_) < 0) return;

    if (event == EVENT_OBJECT_SHOW)
    {
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (!(style & WS_POPUP) || (style & (WS_CHILD | WS_SYSMENU | WS_THICKFRAME)) ||
            (style & WS_CAPTION) == WS_CAPTION || (extended & WS_EX_APPWINDOW) ||
            !self.IsNearAnchor(window)) return;
        if (std::find(self.shown_.begin(), self.shown_.end(), window) != self.shown_.end()) return;
        const auto slot = std::find_if(self.shown_.begin(), self.shown_.end(),
            [&](HWND candidate) { return !self.IsNearAnchor(candidate); });
        if (slot == self.shown_.end()) return;
        *slot = window;
    }
    else if (event == EVENT_OBJECT_HIDE)
    {
        const auto slot = std::find(self.shown_.begin(), self.shown_.end(), window);
        if (slot == self.shown_.end()) return;
        *slot = nullptr;
    }
    else return;

    const UINT changed = HoverPopupChangedMessage();
    if (changed) PostMessageW(self.receiver_, changed, 0, 0);
}
}
