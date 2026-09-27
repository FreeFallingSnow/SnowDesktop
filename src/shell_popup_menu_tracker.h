#pragma once

#include <windows.h>
#include <atomic>

namespace snowdesktop::shell_popup_menu_tracker
{
namespace detail
{
struct WindowContext
{
    HWND forwardingOwner = nullptr;
};

inline LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* context = reinterpret_cast<WindowContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        context = static_cast<WindowContext*>(
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(context));
    }
    if (context && (message == WM_INITMENUPOPUP || message == WM_DRAWITEM ||
            message == WM_MEASUREITEM || message == WM_MENUCHAR) &&
        IsWindow(context->forwardingOwner))
    {
        return SendMessageW(context->forwardingOwner, message, wp, lp);
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace detail

// Shell cascade adapters can depend on the calling thread's active menu state.
// Keep tracking, menu initialization and the context-menu COM object on the
// owner STA. Forwarding only WM_INITMENUPOPUP from another thread is not enough:
// WinRAR's deferred cascade can return S_OK after removing its placeholder,
// leaving an empty submenu. The caller supplies its nested-loop animation pump.
inline UINT Track(HMENU menu, UINT flags, POINT screenPoint, HWND forwardingOwner,
    bool topmost, std::atomic<HWND>& activeOwner,
    const std::atomic<bool>& cancelRequested, HWND nativeOwner = nullptr)
{
    if (!menu || !IsWindow(forwardingOwner) ||
        GetWindowThreadProcessId(forwardingOwner, nullptr) != GetCurrentThreadId())
        return 0;
    // Ownership is only for the invoking Dock's lifetime/Z-order. Shell menu
    // initialization still goes to forwardingOwner on this same STA. Never
    // attach a foreign thread's input queue through an owner relationship.
    if (nativeOwner && (!IsWindow(nativeOwner) ||
        GetWindowThreadProcessId(nativeOwner, nullptr) != GetCurrentThreadId() ||
        (GetWindowLongPtrW(nativeOwner, GWL_STYLE) & WS_CHILD) != 0 ||
        GetAncestor(nativeOwner, GA_ROOT) != nativeOwner ||
        GetParent(nativeOwner) == HWND_MESSAGE))
        return 0;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t className[] = L"SnowDesktopShellMenuTracker";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = detail::WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;

    detail::WindowContext context{ forwardingOwner };
    HWND tracker = CreateWindowExW(WS_EX_TOOLWINDOW | (topmost ? WS_EX_TOPMOST : 0),
        className, L"SnowDesktop Shell Menu Tracker", WS_POPUP,
        -32000, -32000, 1, 1, nativeOwner, nullptr, instance, &context);
    if (!tracker)
        return 0;

    const HWND previousOwner = activeOwner.exchange(tracker, std::memory_order_acq_rel);
    ShowWindow(tracker, SW_SHOWNA);
    SetForegroundWindow(tracker);
    UINT command = 0;
    if (!cancelRequested.load(std::memory_order_acquire))
    {
        command = TrackPopupMenuEx(menu, flags,
            screenPoint.x, screenPoint.y, tracker, nullptr);
    }
    // A nested tracker restores its still-live outer session. If shutdown has
    // already reset/replaced the slot, do not resurrect the old owner.
    HWND expected = tracker;
    activeOwner.compare_exchange_strong(expected,
        previousOwner && IsWindow(previousOwner) ? previousOwner : nullptr,
        std::memory_order_acq_rel);
    DestroyWindow(tracker);
    PostMessageW(forwardingOwner, WM_NULL, 0, 0);
    return command;
}
} // namespace snowdesktop::shell_popup_menu_tracker
