#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>

namespace snowdesktop::shell_execute
{
// Interactive launches originate on the desktop UI thread. Let Shell finish
// shortcut resolution, DDE, or an execution delegate in the background so the
// caller can continue dispatching pointer and foreground messages.
inline constexpr ULONG kInteractiveOpenMask = SEE_MASK_ASYNCOK;

inline bool OpenPathAsync(
    HWND owner,
    const std::wstring& path,
    int showCommand = SW_SHOWNORMAL)
{
    if (path.empty())
        return false;

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = kInteractiveOpenMask;
    executeInfo.hwnd = owner;
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = path.c_str();
    executeInfo.nShow = showCommand;
    return ShellExecuteExW(&executeInfo) != FALSE;
}
}
