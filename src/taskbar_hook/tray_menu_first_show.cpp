#include "tray_menu_first_show.h"

#include <commctrl.h>
#include <algorithm>
#include <cstdint>

namespace snowdesktop::tray::first_show
{
namespace
{
constexpr UINT_PTR kMenuSubclass = 0x53444D46;
struct Watch
{
    Request request;
    HHOOK hook = nullptr;
    UINT_PTR timer = 0;
    HWND candidate = nullptr;
};
thread_local Watch watch;

LRESULT CALLBACK MenuProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void StopWatching()
{
    const auto previous = watch;
    watch = {};
    if (previous.candidate) RemoveWindowSubclass(previous.candidate, MenuProcedure, kMenuSubclass);
    if (previous.timer) KillTimer(nullptr, previous.timer);
    if (previous.hook) UnhookWindowsHookEx(previous.hook);
}

void CALLBACK Expire(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (watch.timer == timer && GetTickCount64() >= watch.request.expires) StopWatching();
}

bool IsWeixinMenu(HWND window)
{
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() ||
        IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
        GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(window)) != DPI_AWARENESS_PER_MONITOR_AWARE)
        return false;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & (WS_CHILD | WS_SYSMENU)) || (style & WS_CAPTION) == WS_CAPTION ||
        !(extended & WS_EX_TOOLWINDOW) || (extended & (WS_EX_APPWINDOW | WS_EX_TRANSPARENT))) return false;
    wchar_t name[64]{};
    GetClassNameW(window, name, static_cast<int>(sizeof(name) / sizeof(name[0])));
    return wcscmp(name, L"Qt51514QWindowPopupSaveBits") == 0;
}

void PlaceAtFirstShow(const Request& request, HWND window, WINDOWPOS& position)
{
    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetWindowRect(window, &bounds) ||
        !GetMonitorInfoW(MonitorFromPoint(request.anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto& work = monitor.rcWork;
    using Coordinate = std::int64_t;
    const Coordinate width = position.flags & SWP_NOSIZE
        ? static_cast<Coordinate>(bounds.right) - bounds.left : position.cx;
    const Coordinate height = position.flags & SWP_NOSIZE
        ? static_cast<Coordinate>(bounds.bottom) - bounds.top : position.cy;
    if (width <= 0 || height <= 0 || width > static_cast<Coordinate>(work.right) - work.left ||
        height > static_cast<Coordinate>(work.bottom) - work.top) return;
    const Coordinate x = position.flags & SWP_NOMOVE ? bounds.left : position.x;
    const Coordinate y = position.flags & SWP_NOMOVE ? bounds.top : position.y;
    constexpr Coordinate proximity = 96;
    const bool nearX = request.anchor.x >= x - proximity && request.anchor.x <= x + width + proximity;
    const bool nearY = request.anchor.y >= y - proximity && request.anchor.y <= y + height + proximity;
    Coordinate desiredX = x, desiredY = y;
    if (!nearX || !nearY)
    {
        if (!nearX) desiredX = request.anchor.x;
        const Coordinate above = request.icon.top - height, below = request.icon.bottom;
        bool preferBelow = static_cast<Coordinate>(request.icon.top) + request.icon.bottom <=
            static_cast<Coordinate>(work.top) + work.bottom;
        if (preferBelow && below + height > work.bottom && above >= work.top) preferBelow = false;
        if (!preferBelow && above < work.top && below + height <= work.bottom) preferBelow = true;
        desiredY = preferBelow ? below : above;
    }
    desiredX = std::clamp(desiredX, static_cast<Coordinate>(work.left), work.right - width);
    desiredY = std::clamp(desiredY, static_cast<Coordinate>(work.top), work.bottom - height);
    if (desiredX == x && desiredY == y) return;
    position.x = static_cast<int>(desiredX);
    position.y = static_cast<int>(desiredY);
    position.flags &= ~static_cast<UINT>(SWP_NOMOVE);
}

LRESULT CALLBACK MenuProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (message == WM_NCDESTROY)
        StopWatching();
    if (message != WM_WINDOWPOSCHANGING || !lp)
        return DefSubclassProc(window, message, wp, lp);
    auto& position = *reinterpret_cast<WINDOWPOS*>(lp);
    if (!(position.flags & SWP_SHOWWINDOW)) return DefSubclassProc(window, message, wp, lp);
    const auto request = watch.request;
    StopWatching();
    const auto result = DefSubclassProc(window, message, wp, lp);
    // Apply after the application handles the message, before User32 commits
    // the move and show together. The menu keeps its original size and z-order.
    if ((position.flags & SWP_SHOWWINDOW) && GetTickCount64() < request.expires && IsWeixinMenu(window))
        PlaceAtFirstShow(request, window, position);
    return result;
}

LRESULT CALLBACK WatchMenu(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && lp && watch.hook)
    {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lp);
        if (GetTickCount64() >= watch.request.expires)
            StopWatching();
        // The hook identifies the pending show. The window procedure below
        // owns the actual edit to the writable WINDOWPOS.
        else if (!watch.candidate && message.message == WM_WINDOWPOSCHANGING && message.lParam &&
            (reinterpret_cast<const WINDOWPOS*>(message.lParam)->flags & SWP_SHOWWINDOW) &&
            GetWindowThreadProcessId(reinterpret_cast<HWND>(watch.request.receiver), nullptr) == GetCurrentThreadId() &&
            IsWeixinMenu(message.hwnd) && SetWindowSubclass(message.hwnd, MenuProcedure, kMenuSubclass, 0))
            watch.candidate = message.hwnd;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}

void Arm(const CWPSTRUCT& message)
{
    if (!message.wParam || !message.lParam ||
        GetWindowThreadProcessId(message.hwnd, nullptr) != GetCurrentThreadId() ||
        !IsWeixinReceiver(message.hwnd)) return;
    const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE,
        RequestName(static_cast<DWORD>(message.wParam), static_cast<ULONG_PTR>(message.lParam)).c_str());
    if (!mapping) return;
    const auto view = static_cast<const Request*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Request)));
    const auto request = view ? *view : Request{};
    if (view) UnmapViewOfFile(view);
    CloseHandle(mapping);
    const auto now = GetTickCount64();
    if (request.receiver != reinterpret_cast<UINT_PTR>(message.hwnd) ||
        request.expires <= now || request.expires - now > kLifetimeMs ||
        IsRectEmpty(&request.icon) || !PtInRect(&request.icon, request.anchor)) return;

    StopWatching();
    // The local hook and its timeout can outlive the remote arm message.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&WatchMenu), &pinned)) return;
    watch.request = request;
    watch.hook = SetWindowsHookExW(WH_CALLWNDPROC, WatchMenu, nullptr, GetCurrentThreadId());
    if (watch.hook) watch.timer = SetTimer(nullptr, 0, kLifetimeMs, Expire);
    if (!watch.timer) StopWatching();
}
}
