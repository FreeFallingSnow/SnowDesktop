#include "platform/foreground_fullscreen.h"

#include <iostream>

int main()
{
    using namespace snowdesktop::fullscreen;
    int failures = 0;
    const auto require = [&](bool result, const char* message) {
        if (!result) { std::cerr << message << '\n'; ++failures; }
    };
    const RECT monitor{0, 0, 2560, 1440};
    // Observed Steam Big Picture SDL_app, 2026-11-03: both rectangles stop
    // one physical pixel above the monitor bottom; it is not maximized.
    WindowSample steam;
    steam.window = reinterpret_cast<HWND>(1);
    steam.style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_MINIMIZEBOX;
    steam.extended = WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    steam.client = steam.frame = {0, 0, 2560, 1439};
    require(IsFullscreen(steam, monitor), "Steam Big Picture one-pixel-short window is fullscreen");

    auto window = steam;
    window.client = window.frame = monitor;
    require(IsFullscreen(window, monitor), "exact monitor coverage remains fullscreen");
    window.frame = monitor;
    window.client.bottom -= 2;
    require(IsFullscreen(window, monitor), "Terminal two-pixel client inset remains fullscreen");
    window.frame.bottom -= 1;
    require(IsFullscreen(window, monitor), "client and frame allowances share the monitor origin");
    window.client.bottom -= 1;
    require(!IsFullscreen(window, monitor), "frame and client allowances cannot accumulate");
    window = steam;
    window.frame.bottom -= 1;
    require(!IsFullscreen(window, monitor), "outer frame two pixels short is rejected");
    window = steam;
    window.client = window.frame = {0, 40, 2560, 1440};
    require(!IsFullscreen(window, monitor), "window limited to the actual top-AppBar work area is rejected");
    window.client = window.frame = {0, 0, 2560, 1400};
    require(!IsFullscreen(window, monitor), "window above an ordinary bottom taskbar is rejected");
    window = steam;
    window.style |= WS_OVERLAPPEDWINDOW;
    require(!IsFullscreen(window, monitor), "framed near-fullscreen window is rejected");
    window.maximized = true;
    window.client = window.frame = monitor;
    require(!IsFullscreen(window, monitor), "ordinary maximization is rejected even at monitor size");
    window = steam;
    window.maximized = true;
    require(IsFullscreen(window, monitor), "frameless Chromium fullscreen may retain maximized state");
    window = steam;
    window.cloaked = true;
    require(!IsFullscreen(window, monitor), "cloaked source is rejected");
    window = steam;
    window.nonRude = true;
    require(!IsFullscreen(window, monitor), "passive NonRude surface is rejected");
    window = steam;
    window.shell = true;
    require(!IsFullscreen(window, monitor), "Shell surface is rejected");
    for (const auto flag : {WS_EX_NOACTIVATE, WS_EX_TRANSPARENT})
    {
        window = steam;
        window.extended |= flag;
        require(!IsFullscreen(window, monitor), "passive extended style is rejected");
    }
    window = steam;
    window.frame = {};
    require(!IsFullscreen(window, monitor), "near-fullscreen client requires valid outer-frame evidence");
    window.client = monitor;
    require(IsFullscreen(window, monitor), "strict client coverage survives missing outer-frame evidence");
    window.client = {};
    require(!IsFullscreen(window, monitor), "empty client is rejected");
    require(!IsFullscreen(steam, RECT{}), "empty monitor is rejected");

    window = steam;
    window.client = window.frame = {-2560, -1440, 0, -1};
    const RECT secondary{-2560, -1440, 0, 0};
    require(IsFullscreen(window, secondary), "negative-coordinate monitor uses the same physical-pixel tolerance");
    const std::array<std::optional<RECT>, 2> monitors{monitor, secondary};
    auto sources = ScanMonitors(monitors, [&](const auto& visit) {
        visit(steam); visit(window); return true;
    });
    require(sources[0] && sources[0]->window == steam.window &&
        sources[1] && sources[1]->window == window.window,
        "per-monitor observer reports the near-fullscreen sources");
    auto foreground = steam;
    foreground.window = reinterpret_cast<HWND>(2);
    foreground.client = foreground.frame = {100, 100, 2000, 1200};
    sources = ScanMonitors(monitors, [&](const auto& visit) {
        if (visit(foreground)) visit(steam);
        return true;
    });
    require(sources[0] && !sources[0]->window,
        "ordinary foreground app still obscures a background fullscreen source");
    bool activated = false;
    require(!GuardFocusRequest(false, [&] { return IsFullscreen(steam, monitor); },
        [&] { activated = true; }) && !activated,
        "near-fullscreen source protects focus from unsolicited activation");
    require(GuardFocusRequest(true, [&] { return IsFullscreen(steam, monitor); },
        [&] { activated = true; }) && activated,
        "explicit fullscreen exit can still request focus");
    std::cout << "Fullscreen geometry, monitor scan and focus protection: " << failures << " failures\n";
    return failures ? 1 : 0;
}
