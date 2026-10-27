#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <cwchar>

namespace snowdesktop::start_menu
{
inline constexpr wchar_t kCoreWindowClass[] = L"Windows.UI.Core.CoreWindow";

struct WindowDiscovery
{
    unsigned candidates = 0, hosts = 0, hidden = 0;
    DWORD processError = ERROR_SUCCESS, cloak = 0;
    HRESULT cloakError = S_OK;
    HWND lastHost = nullptr;
    bool scanLimited = false;
};

inline bool IsStartWindow(HWND window, DWORD* error = nullptr) noexcept
{
    wchar_t type[96]{};
    if (!GetClassNameW(window, type, static_cast<int>(std::size(type))) ||
        _wcsicmp(type, kCoreWindowClass) != 0) return false;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(window, &process)) return false;
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) { if (error) *error = GetLastError(); return false; }
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(handle, 0, path, &length) != FALSE;
    const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!queried) { if (error) *error = queryError; return false; }
    const wchar_t* filename = wcsrchr(path, L'\\');
    // Win10 1903+ and Win11 use this host. Older ShellExperienceHost also
    // owns unrelated panels; never attach on the basis of the class alone.
    return filename && _wcsicmp(filename + 1, L"StartMenuExperienceHost.exe") == 0;
}

// Keep the traversal bounded even if the Shell creates/destroys/reorders
// windows while we inspect them. Only the Win32 boundary is substituted by
// the isolated tests; selection and termination are the production path.
template<class NextWindow, class InspectWindow>
HWND FindVisibleStartWindow(WindowDiscovery& discovery, NextWindow next, InspectWindow inspect)
{
    discovery = {};
    std::array<HWND, 128> visited{};
    HWND after = nullptr;
    for (auto& entry : visited)
    {
        const HWND window = next(after);
        if (!window) return nullptr;
        if (std::find(visited.begin(), visited.end(), window) != visited.end())
        { discovery.scanLimited = true; return nullptr; }
        entry = window;
        ++discovery.candidates;
        if (inspect(window, discovery)) return window;
        after = window;
    }
    discovery.scanLimited = true;
    return nullptr;
}

inline HWND FindVisibleStartWindow(WindowDiscovery& discovery)
{
    // Windhawk's Shell Flyout Positions locates CoreWindow from INSIDE the
    // Start host. Our desktop process must not copy its EnumWindows call:
    // https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-enumwindows
    // filters immersive windows by default. Direct class lookup avoids that
    // filter without changing enumeration behavior throughout SnowDesktop.
    return FindVisibleStartWindow(discovery,
        [](HWND after) { return FindWindowExW(nullptr, after, kCoreWindowClass, nullptr); },
        [](HWND window, WindowDiscovery& result) {
            if (!IsStartWindow(window, &result.processError)) return false;
            ++result.hosts;
            result.lastHost = window;
            result.cloak = 0;
            result.cloakError = DwmGetWindowAttribute(window, DWMWA_CLOAKED,
                &result.cloak, sizeof(result.cloak));
            const bool visible = IsWindowVisible(window) && SUCCEEDED(result.cloakError) && !result.cloak;
            if (!visible) ++result.hidden;
            return visible;
        });
}
}
