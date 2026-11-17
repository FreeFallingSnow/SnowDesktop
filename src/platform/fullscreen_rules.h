#pragma once

#include "foreground_fullscreen.h"
#include <functional>

namespace snowdesktop::fullscreen
{
inline bool CoversMonitorCenter(const RECT& bounds, const RECT& monitor) noexcept
{
    const POINT center{monitor.left + (monitor.right - monitor.left) / 2,
        monitor.top + (monitor.bottom - monitor.top) / 2};
    return PtInRect(&bounds, center) != FALSE;
}

// nullopt means an unknown monitor/failed observation; an empty WindowSample
// means the scan succeeded and found no unobscured fullscreen source.
template<class Enumerate>
std::vector<std::optional<WindowSample>> ScanMonitors(
    std::span<const std::optional<RECT>> monitors, Enumerate&& enumerate, HWND desktopRoot = nullptr)
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
            // Show Desktop can move an unminimized game behind the desktop.
            // Only the desktop root is a boundary, on screens it covers.
            if (desktopRoot && window.window == desktopRoot && CoversMonitorCenter(window.frame, monitor))
            {
                settled[i] = true;
                --remaining;
                continue;
            }
            if (IsFullscreen(window, monitor))
            {
                sources[i] = window;
                settled[i] = true;
                --remaining;
                continue;
            }
            // An ordinary application covering this screen's center obscures
            // a background fullscreen source. Passive/desktop surfaces do not.
            if (!window.shell && !window.cloaked && !window.nonRude && CoversMonitorCenter(window.client, monitor) &&
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


} // namespace snowdesktop::fullscreen
