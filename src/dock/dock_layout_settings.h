#pragma once

#include <algorithm>
#include <cmath>

inline constexpr bool kDefaultDockEnabled = false;
inline constexpr float kDockMinimumScale = 0.50f;
inline constexpr float kDockMaximumScale = 2.00f;
inline constexpr float kDockMinimumEdgeDistanceScale = 0.10f;
inline constexpr float kDockMaximumEdgeDistanceScale = 5.00f;

inline float ClampDockEdgeDistanceScale(float scale)
{
    return std::isfinite(scale)
        ? std::clamp(scale, kDockMinimumEdgeDistanceScale, kDockMaximumEdgeDistanceScale)
        : 1.0f;
}

inline int ScaleDockEdgeDistance(int baseMargin, float scale)
{
    if (baseMargin <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(
        baseMargin * ClampDockEdgeDistanceScale(scale))));
}

inline float ClampMergedDockIconScale(float scale)
{
    return std::clamp(scale, kDockMinimumScale, 1.00f);
}

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
    // Island-only multiplier; 100% preserves the desktop component margin.
    float edgeDistanceScale = 1.0f;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    bool showWindowsButton = true;
    bool showQuickSearchButton = true;
    bool showFrequentItems = false;
    bool keepWhenDesktopHidden = false;
    bool allowDesktopContentOverlap = false;
    bool reserveScreenSpace = false;
    bool lastMonitorUseHomeSize = true;
    // Independent of page density, Dock icon scale and standalone status scale.
    int mergedBarHeight = 48;
    bool showOnlyWhenSummoned = false;
    int frequentItemCount = 3;
    float thicknessScale = 1.0f;
    float mergedIconScale = 1.0f;
};

inline float ResolveDockScale(const DockLayoutSettings& settings, bool merged)
{
    return merged ? ClampMergedDockIconScale(settings.mergedIconScale) : ClampDockScale(settings.thicknessScale);
}
