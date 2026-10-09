#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <optional>
#include <iterator>
#include <utility>

namespace snowdesktop::fullscreen
{
inline bool ClientCoversMonitor(const RECT& client, const RECT& monitor) noexcept
{
    return monitor.right > monitor.left && monitor.bottom > monitor.top &&
        client.right > client.left && client.bottom > client.top &&
        client.left <= monitor.left && client.top <= monitor.top &&
        client.right >= monitor.right && client.bottom >= monitor.bottom;
}

struct WindowSnapshot
{
    HWND window = nullptr;
    HMONITOR monitor = nullptr;
    DWORD process = 0;
    RECT client{}, bounds{};
};

// Window geometry cannot identify games, videos, browser content or a remote
// swap chain's exclusive state. Protect every foreground fullscreen client.
// No background enumeration, process-name guesses or injection into games.
inline std::optional<WindowSnapshot> ObserveWindow(
    HWND window, DWORD ignoredProcess = GetCurrentProcessId()) noexcept
{
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return std::nullopt;
    WindowSnapshot snapshot;
    snapshot.window = window;
    GetWindowThreadProcessId(window, &snapshot.process);
    if (!snapshot.process || snapshot.process == ignoredProcess) return std::nullopt;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) ||
        (IsZoomed(window) && (style & WS_CAPTION) == WS_CAPTION) ||
        GetPropW(window, L"NonRudeHWND")) return std::nullopt;
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (_wcsicmp(name, L"Progman") == 0 || _wcsicmp(name, L"WorkerW") == 0 ||
        _wcsicmp(name, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(name, L"Shell_SecondaryTrayWnd") == 0) return std::nullopt;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return std::nullopt;
    snapshot.monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!snapshot.monitor || !GetMonitorInfoW(snapshot.monitor, &info) ||
        !GetClientRect(window, &snapshot.client)) return std::nullopt;
    POINT origin{snapshot.client.left, snapshot.client.top};
    POINT end{snapshot.client.right, snapshot.client.bottom};
    if (!ClientToScreen(window, &origin) || !ClientToScreen(window, &end)) return std::nullopt;
    snapshot.client = {origin.x, origin.y, end.x, end.y};
    snapshot.bounds = info.rcMonitor;
    return ClientCoversMonitor(snapshot.client, snapshot.bounds)
        ? std::optional<WindowSnapshot>{snapshot} : std::nullopt;
}

inline std::optional<WindowSnapshot> ObserveForeground() noexcept
{
    return ObserveWindow(GetForegroundWindow());
}

// Keep the check at the side-effect boundary, including any attached-input
// retry. Tests can substitute the observation and activation without touching
// the user's input desktop or activating the production desktop host.
template<class Observe, class Activate>
bool GuardFocusRequest(bool deliberateFullscreenExit, Observe&& observe, Activate&& activate)
{
    if (!deliberateFullscreenExit && observe()) return false;
    std::forward<Activate>(activate)();
    return true;
}
} // namespace snowdesktop::fullscreen
