#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <array>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

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
    // Chromium retains WS_MAXIMIZE after removing its caption and resize frame.
    return maximized && (style & (WS_CAPTION | WS_THICKFRAME)) != 0;
}

inline bool IsCandidate(LONG_PTR style, LONG_PTR extended, bool maximized, bool shell) noexcept
{
    return !shell && !(extended & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) &&
        !IsWindowedMaximization(style, maximized);
}

// A native window is sampled once, then shared by all monitors in a scan.
// Invisible, minimized and ignored-process windows never enter the scan.
struct WindowSample
{
    HWND window = nullptr;
    RECT client{};
    LONG_PTR style = 0, extended = 0;
    bool maximized = false, cloaked = false, shell = false;
    RECT frame{};
    bool nonRude = false;
    DWORD process = 0;
};

inline bool IsFullscreen(const WindowSample& window, const RECT& monitor) noexcept
{
    if (window.cloaked || window.nonRude ||
        !IsCandidate(window.style, window.extended, window.maximized, window.shell)) return false;
    if (ClientCoversMonitor(window.client, monitor)) return true;
    // Windows Terminal reserves two physical pixels for an auto-hidden taskbar,
    // even in fullscreen. Accept only a frameless, monitor-covering outer window
    // with at most this tiny client inset, never a work-area-sized window.
    constexpr LONG edgeInset = 2;
    const auto& client = window.client;
    return !(window.style & (WS_CAPTION | WS_THICKFRAME)) &&
        ClientCoversMonitor(window.frame, monitor) &&
        client.right > client.left && client.bottom > client.top &&
        static_cast<LONGLONG>(client.left) <= static_cast<LONGLONG>(monitor.left) + edgeInset &&
        static_cast<LONGLONG>(client.top) <= static_cast<LONGLONG>(monitor.top) + edgeInset &&
        static_cast<LONGLONG>(client.right) >= static_cast<LONGLONG>(monitor.right) - edgeInset &&
        static_cast<LONGLONG>(client.bottom) >= static_cast<LONGLONG>(monitor.bottom) - edgeInset;
}

inline std::optional<WindowSample> SampleWindow(
    HWND window, DWORD ignoredProcess = GetCurrentProcessId()) noexcept
{
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return std::nullopt;
    WindowSample sample;
    sample.window = window;
    GetWindowThreadProcessId(window, &sample.process);
    if (!sample.process || sample.process == ignoredProcess) return std::nullopt;
    sample.style = GetWindowLongPtrW(window, GWL_STYLE);
    sample.extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    sample.maximized = IsZoomed(window) != FALSE;
    sample.nonRude = GetPropW(window, L"NonRudeHWND") != nullptr;
    if ((sample.extended & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) || sample.nonRude)
        return std::nullopt;
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    sample.shell = _wcsicmp(name, L"Progman") == 0 || _wcsicmp(name, L"WorkerW") == 0 ||
        _wcsicmp(name, L"Shell_TrayWnd") == 0 || _wcsicmp(name, L"Shell_SecondaryTrayWnd") == 0;
    if (sample.shell) return std::nullopt;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        sample.cloaked = cloaked != 0;
    if (sample.cloaked || !GetClientRect(window, &sample.client)) return std::nullopt;
    POINT origin{sample.client.left, sample.client.top}, end{sample.client.right, sample.client.bottom};
    if (!ClientToScreen(window, &origin) || !ClientToScreen(window, &end)) return std::nullopt;
    sample.client = {origin.x, origin.y, end.x, end.y};
    // A failed outer-frame query still permits strict client coverage.
    if (!GetWindowRect(window, &sample.frame)) sample.frame = {};
    return sample;
}

struct WindowSnapshot
{
    HWND window = nullptr;
    HMONITOR monitor = nullptr;
    DWORD process = 0;
    RECT client{}, bounds{};
};

// Geometry cannot identify content or another process's exclusive swap chain.
// Callers own their reveal/focus/pause policies; all use this same observation.
inline std::optional<WindowSnapshot> ObserveWindow(
    HWND window, DWORD ignoredProcess = GetCurrentProcessId()) noexcept
{
    const auto sample = SampleWindow(window, ignoredProcess);
    if (!sample) return std::nullopt;
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info) || !IsFullscreen(*sample, info.rcMonitor))
        return std::nullopt;
    return WindowSnapshot{window, monitor, sample->process, sample->client, info.rcMonitor};
}

inline std::optional<WindowSnapshot> ObserveForeground() noexcept
{
    return ObserveWindow(GetForegroundWindow());
}

// nullopt means an unknown monitor/failed observation; an empty WindowSample
// means the scan succeeded and found no unobscured fullscreen source.
template<class Enumerate>
std::vector<std::optional<WindowSample>> ScanMonitors(
    std::span<const std::optional<RECT>> monitors, Enumerate&& enumerate)
{
    std::vector<std::optional<WindowSample>> sources(monitors.size());
    std::vector<bool> settled(monitors.size(), false);
    std::size_t remaining = 0;
    for (std::size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i]) { sources[i] = WindowSample{}; ++remaining; }
    if (!remaining) return sources;
    const std::function<bool(const WindowSample&)> visit = [&](const auto& window) {
        for (std::size_t i = 0; i < monitors.size(); ++i)
        {
            if (!monitors[i] || settled[i]) continue;
            const RECT monitor = *monitors[i];
            if (IsFullscreen(window, monitor))
            {
                sources[i] = window;
                settled[i] = true;
                --remaining;
                continue;
            }
            // An ordinary application covering this screen's center obscures
            // a background fullscreen source. Passive/desktop surfaces do not.
            const POINT center{monitor.left + (monitor.right - monitor.left) / 2,
                monitor.top + (monitor.bottom - monitor.top) / 2};
            if (!window.shell && !window.cloaked && !window.nonRude && PtInRect(&window.client, center) &&
                !(window.extended & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)))
            {
                settled[i] = true;
                --remaining;
            }
        }
        return remaining != 0;
    };
    if (!enumerate(visit))
        for (std::size_t i = 0; i < sources.size(); ++i)
            if (!settled[i]) sources[i].reset();
    return sources;
}

inline std::vector<std::optional<WindowSnapshot>> ObserveMonitors(
    std::span<const HMONITOR> monitors, DWORD ignoredProcess = GetCurrentProcessId())
{
    std::vector<std::optional<RECT>> bounds;
    bounds.reserve(monitors.size());
    for (const auto monitor : monitors)
    {
        MONITORINFO info{sizeof(info)};
        bounds.push_back(monitor && GetMonitorInfoW(monitor, &info) ? std::optional{info.rcMonitor} : std::nullopt);
    }
    const auto samples = ScanMonitors(bounds, [&](const auto& visit) {
        struct Context { const std::function<bool(const WindowSample&)>& visit; DWORD ignoredProcess; };
        Context context{visit, ignoredProcess};
        return EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            const auto& state = *reinterpret_cast<const Context*>(parameter);
            const auto sample = SampleWindow(window, state.ignoredProcess);
            return !sample || state.visit(*sample) ? TRUE : FALSE;
        }, reinterpret_cast<LPARAM>(&context)) != FALSE;
    });
    std::vector<std::optional<WindowSnapshot>> sources(monitors.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
        if (samples[i]) sources[i] = WindowSnapshot{samples[i]->window, monitors[i],
            samples[i]->process, samples[i]->client, *bounds[i]};
    return sources;
}

// A Dock can live on another monitor than the foreground/minimized window.
inline std::optional<WindowSnapshot> ObserveMonitor(
    HMONITOR monitor, DWORD ignoredProcess = GetCurrentProcessId())
{
    const std::array monitors{monitor};
    auto source = ObserveMonitors(monitors, ignoredProcess).front();
    return source && source->window ? source : std::nullopt;
}

// Re-observe at the side-effect boundary, including any attached-input retry.
template<class Observe, class Activate>
bool GuardFocusRequest(bool deliberateFullscreenExit, Observe&& observe, Activate&& activate)
{
    if (!deliberateFullscreenExit && observe()) return false;
    std::forward<Activate>(activate)();
    return true;
}
} // namespace snowdesktop::fullscreen
