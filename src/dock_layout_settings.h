#pragma once

#include <algorithm>

inline constexpr bool kDefaultDockEnabled = false;
inline constexpr float kDockMinimumScale = 0.50f;
inline constexpr float kDockMaximumScale = 1.00f;

inline float ClampDockScale(float scale)
{
    return std::clamp(scale, kDockMinimumScale, kDockMaximumScale);
}

enum class DockPosition
{
    Bottom = 0,
    Top = 1,
    Left = 2,
    Right = 3
};

enum class DockMonitorScope
{
    First = 0,
    Last = 1,
    All = 2
};

// The layout-owned part of Dock settings. Appearance, animation, invocation
// shortcuts and Windows taskbar preferences remain independent of layouts.
struct DockLayoutSettings
{
    bool operator==(const DockLayoutSettings&) const = default;

    DockPosition position = DockPosition::Bottom;
    bool edgeAttached = false;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    bool showWindowsButton = true;
    bool showFrequentItems = false;
    bool keepWhenDesktopHidden = false;
    bool allowDesktopContentOverlap = false;
    bool showOnlyWhenSummoned = false;
    int frequentItemCount = 3;
    float thicknessScale = 1.0f;
};
