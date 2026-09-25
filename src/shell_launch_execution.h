#pragma once

#include "shell_launch_process.h"
#include <shellapi.h>

namespace snowdesktop::shell_launch_process
{
// Internal Win32 boundary, shared by the real helper and deterministic consent
// regressions. Tests replace only OS foreground/consent calls, not routing.
struct ExecutionApi
{
    using OpenExecutor = bool (*)(HWND, const std::wstring&,
        PCIDLIST_ABSOLUTE, int, ULONG);
    decltype(&GetForegroundWindow) foreground = &GetForegroundWindow;
    decltype(&SetForegroundWindow) activate = &SetForegroundWindow;
    decltype(&AllowSetForegroundWindow) allow = &AllowSetForegroundWindow;
    decltype(&ShellExecuteExW) execute = &ShellExecuteExW;
    // A null Open boundary selects the production Shell implementation.
    OpenExecutor open = nullptr;
};

inline bool IsLaunchOwner(HWND window)
{
    return window && IsWindow(window) && IsWindowVisible(window) &&
        IsWindowEnabled(window) &&
        !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) &&
        !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
}

// The hidden tray control and Explorer-owned render child cannot anchor UAC.
// Preserve explicit top-level callers; desktop launches use the durable input
// proxy, which survives dismissal of menus and quick-navigation panels.
inline HWND ResolveLaunchOwner(HWND requested, HWND desktopRender, HWND desktopInput)
{
    if (requested != desktopRender && IsLaunchOwner(requested))
        return requested;
    return IsLaunchOwner(desktopInput) ? desktopInput : nullptr;
}

bool ExecuteRequestWithApi(const Request& request, const ExecutionApi& api);
}
