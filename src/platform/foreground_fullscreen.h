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

inline bool IsWindowedMaximization(LONG_PTR style, bool maximized) noexcept
{
    // Chromium retains WS_MAXIMIZE when fullscreen removes the caption and
    // resize frame. Custom-chrome maximization retains its resize frame even
    // without WS_CAPTION. Geometry still has to cover the complete monitor.
    return maximized && (style & (WS_CAPTION | WS_THICKFRAME)) != 0;
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
        IsWindowedMaximization(style, IsZoomed(window) != FALSE) ||
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

// A Dock can live on another monitor than the minimized or foreground window.
// Observe that monitor's visible application layer, ignoring our own surfaces.
inline std::optional<WindowSnapshot> ObserveMonitor(
    HMONITOR monitor, DWORD ignoredProcess = GetCurrentProcessId()) noexcept
{
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return std::nullopt;
    struct Context
    {
        HMONITOR monitor;
        RECT bounds;
        DWORD ignoredProcess;
        std::optional<WindowSnapshot> result;
    } context{monitor, info.rcMonitor, ignoredProcess, {}};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(parameter);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (!process || process == state.ignoredProcess || !IsWindowVisible(window) || IsIconic(window))
            return TRUE;
        const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((extended & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) || GetPropW(window, L"NonRudeHWND"))
            return TRUE;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            return TRUE;
        wchar_t name[128]{};
        GetClassNameW(window, name, static_cast<int>(std::size(name)));
        if (_wcsicmp(name, L"Progman") == 0 || _wcsicmp(name, L"WorkerW") == 0 ||
            _wcsicmp(name, L"Shell_TrayWnd") == 0 || _wcsicmp(name, L"Shell_SecondaryTrayWnd") == 0)
            return TRUE;
        if (auto observed = ObserveWindow(window, state.ignoredProcess);
            observed && ClientCoversMonitor(observed->client, state.bounds))
        {
            observed->monitor = state.monitor;
            observed->bounds = state.bounds;
            state.result = observed;
            return FALSE;
        }
        // A normal application covering this monitor's center obscures a
        // background fullscreen client. Tool windows do not end that client.
        RECT client{};
        POINT origin{}, end{};
        if (!(extended & WS_EX_TOOLWINDOW) && GetClientRect(window, &client))
        {
            origin = {client.left, client.top}; end = {client.right, client.bottom};
            if (ClientToScreen(window, &origin) && ClientToScreen(window, &end))
            {
                client = {origin.x, origin.y, end.x, end.y};
                const POINT center{state.bounds.left + (state.bounds.right - state.bounds.left) / 2,
                    state.bounds.top + (state.bounds.bottom - state.bounds.top) / 2};
                if (PtInRect(&client, center)) return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.result;
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
