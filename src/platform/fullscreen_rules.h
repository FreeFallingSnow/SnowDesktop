#pragma once

#include "foreground_fullscreen.h"
#include <functional>

namespace snowdesktop::fullscreen
{
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


} // namespace snowdesktop::fullscreen
