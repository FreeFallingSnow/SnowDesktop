#pragma once
#include "status_bar_appbar.h"
#include <span>
#include <vector>

namespace snowdesktop
{
// One native window sample is consumed in Z order by every unresolved monitor.
// Invisible, minimized and application-owned windows are filtered by the native
// collector before sampling their client rectangle, as in the single-bar path.
struct StatusBarFullscreenWindow
{
    HWND window = nullptr;
    RECT client{};
    LONG_PTR style = 0, extended = 0;
    bool maximized = false, cloaked = false, shell = false;
};

template<class Enumerate>
std::vector<std::optional<HWND>> ObserveStatusBarFullscreen(
    std::span<const std::optional<RECT>> monitors, Enumerate&& enumerate)
{
    std::vector<std::optional<HWND>> sources(monitors.size());
    std::vector<bool> settled(monitors.size(), false);
    std::size_t remaining = 0;
    for (std::size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i]) { sources[i] = HWND{}; ++remaining; }
    if (!remaining) return sources;

    const std::function<bool(const StatusBarFullscreenWindow&)> visit = [&](const auto& window) {
        const bool candidate = StatusBarFullscreenCandidate(window.style, window.extended,
            window.maximized, window.shell);
        for (std::size_t i = 0; i < monitors.size(); ++i)
        {
            if (!monitors[i] || settled[i]) continue;
            const RECT monitor = *monitors[i];
            if (candidate && StatusBarFullscreenClient(window.client, monitor,
                true, false, window.cloaked, window.shell))
            {
                sources[i] = window.window;
                settled[i] = true;
                --remaining;
                continue;
            }
            // A regular foreground window covering this screen's center hides
            // a background fullscreen source. Other screens remain unresolved.
            const POINT center{monitor.left + (monitor.right - monitor.left) / 2,
                monitor.top + (monitor.bottom - monitor.top) / 2};
            if (!window.shell && !window.cloaked && PtInRect(&window.client, center) &&
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

// A pending message contains only its receiving HWND, never a Window pointer.
// A failed post remains retryable; removing its receiver permits handoff.
class StatusBarFullscreenRequest
{
public:
    template<class Post>
    bool Queue(HWND window, Post&& post)
    {
        if (!window || recipient_) return false;
        if (!post(window)) return false;
        recipient_ = window;
        return true;
    }
    bool Consume(HWND window)
    {
        if (!window || recipient_ != window) return false;
        recipient_ = nullptr;
        return true;
    }
private:
    HWND recipient_ = nullptr;
};
}
