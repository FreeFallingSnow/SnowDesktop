#pragma once

#include <windows.h>
#include <shlobj.h>
#include <optional>
#include <string>

struct ShellChangeNotification
{
    LONG event = 0;
    std::wstring source;
    std::wstring target;
};

inline ULONG RegisterDesktopShellNotifications(HWND window, UINT message,
    PCIDLIST_ABSOLUTE desktop, PCIDLIST_ABSOLUTE recycleBin)
{
    // A null PIDL subscribes to every path, not to the desktop. Startup can
    // register before its first asynchronous desktop snapshot initializes it.
    const ITEMIDLIST desktopRoot{};
    const SHChangeNotifyEntry entries[] = {
        {desktop ? desktop : &desktopRoot, FALSE}, {recycleBin, TRUE}};
    return SHChangeNotifyRegister(window,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
            SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
            SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES | SHCNE_ASSOCCHANGED,
        message, recycleBin ? 2 : 1, entries);
}

inline std::optional<ShellChangeNotification> ReadShellChangeNotification(
    WPARAM wp, LPARAM lp)
{
    // SHCNRF_NewDelivery: wParam is the shared-memory handle, lParam is
    // the originating process ID. Unlock the HLOCK returned by Lock.
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    const HANDLE lock = wp ? SHChangeNotification_Lock(
        reinterpret_cast<HANDLE>(wp), static_cast<DWORD>(lp), &pidls, &event) : nullptr;
    if (!lock)
        return std::nullopt;
    struct Unlock
    {
        HANDLE value;
        ~Unlock() { SHChangeNotification_Unlock(value); }
    } unlock{lock};
    ShellChangeNotification result;
    result.event = event;
    constexpr LONG pathEvents = SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER |
        SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
        SHCNE_UPDATEITEM | SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES;
    if (pidls && (event & pathEvents) != 0)
    {
        wchar_t source[MAX_PATH]{};
        wchar_t target[MAX_PATH]{};
        if (pidls[0] && SHGetPathFromIDListW(pidls[0], source))
            result.source = source;
        if ((event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER)) != 0 &&
            pidls[1] && SHGetPathFromIDListW(pidls[1], target))
            result.target = target;
    }
    return result;
}
