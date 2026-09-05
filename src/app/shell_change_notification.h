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
    if (pidls && (event == SHCNE_RENAMEITEM || event == SHCNE_RENAMEFOLDER))
    {
        wchar_t source[MAX_PATH]{};
        wchar_t target[MAX_PATH]{};
        if (pidls[0] && pidls[1] &&
            SHGetPathFromIDListW(pidls[0], source) &&
            SHGetPathFromIDListW(pidls[1], target))
        {
            result.source = source;
            result.target = target;
        }
    }
    return result;
}
