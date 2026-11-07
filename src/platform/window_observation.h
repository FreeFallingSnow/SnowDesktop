#pragma once

#include "platform/foreground_fullscreen.h"
#include <unordered_map>

namespace snowdesktop::window_observation
{
struct WindowPlacement
{
    HMONITOR monitor = nullptr;
    bool maximized = false;
    bool fullscreenSized = false;

    bool operator==(const WindowPlacement&) const = default;
};

struct MonitorState
{
    bool visible = false;
    bool maximized = false;
    // An empty optional is unknown. A null HWND is a known clear monitor.
    std::optional<HWND> fullscreenSource;
    RECT desktopCoverage{};
};

struct Snapshot
{
    std::unordered_map<HMONITOR, MonitorState> monitors;
    std::unordered_map<HWND, WindowPlacement> placements;
    std::vector<RECT> desktopCoverage;
};

// Page and window bounds use physical screen coordinates.
Snapshot Observe(std::span<const RECT> pages, HWND desktopRoot);
std::optional<WindowPlacement> ObservePlacement(HWND window) noexcept;

// Recheck the known source and foreground at an interaction boundary without
// starting a second enumeration or applying a stale app's exception.
std::optional<fullscreen::WindowSnapshot> RecheckFullscreenSource(
    HMONITOR monitor, HWND knownSource);
}
