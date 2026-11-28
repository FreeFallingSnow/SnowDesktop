#pragma once

#include <cstddef>
#include <optional>

inline constexpr bool kDefaultDockEnabled = false;
inline constexpr float kDockMinimumScale = 0.50f;
inline constexpr float kDockMaximumScale = 2.00f;
inline constexpr float kDockMinimumEdgeDistanceScale = 0.10f;
inline constexpr float kDockMaximumEdgeDistanceScale = 5.00f;

float ClampDockEdgeDistanceScale(float scale);

int ScaleDockEdgeDistance(int baseMargin, float scale);

float ClampMergedDockIconScale(float scale);

float ClampDockScale(float scale);

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

enum class BarMonitorRole { First, Last, Other };

constexpr BarMonitorRole GetBarMonitorRole(std::size_t index, std::size_t count) noexcept
{
    if (index == 0) return BarMonitorRole::First;
    return index + 1 == count ? BarMonitorRole::Last : BarMonitorRole::Other;
}

// The layout-owned part of Dock settings. Appearance, animation, invocation
// shortcuts and Windows taskbar preferences remain independent of layouts.
struct DockLayoutSettings
{
    bool operator==(const DockLayoutSettings&) const = default;

    DockPosition position = DockPosition::Bottom;
    bool edgeAttached = false;
    bool edgeCentered = false;
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
    // Only monitor overrides can disable the running area.
    bool showRunningArea = true;
};

struct DockMonitorOverride
{
    bool operator==(const DockMonitorOverride&) const = default;
    std::optional<DockPosition> position;
    std::optional<bool> edgeAttached, edgeCentered;
    std::optional<float> thicknessScale;
    std::optional<bool> showOnlyWhenSummoned, allowDesktopContentOverlap, reserveScreenSpace;
    std::optional<bool> showFrequentItems, keepWhenDesktopHidden, showRunningArea;
    std::optional<int> hoverEffect, launchEffect, windowEffect;
    std::optional<float> hoverScale;

    template <typename Self, typename Visitor>
    static void Visit(Self& value, Visitor&& visit)
    {
        visit("position", value.position);
        visit("edgeAttached", value.edgeAttached);
        visit("edgeCentered", value.edgeCentered);
        visit("thicknessScale", value.thicknessScale);
        visit("showOnlyWhenSummoned", value.showOnlyWhenSummoned);
        visit("allowDesktopContentOverlap", value.allowDesktopContentOverlap);
        visit("reserveScreenSpace", value.reserveScreenSpace);
        visit("showFrequentItems", value.showFrequentItems);
        visit("keepWhenDesktopHidden", value.keepWhenDesktopHidden);
        visit("showRunningArea", value.showRunningArea);
        visit("hoverEffect", value.hoverEffect);
        visit("hoverScale", value.hoverScale);
        visit("launchEffect", value.launchEffect);
        visit("windowEffect", value.windowEffect);
    }
};

struct DockMonitorLayout : DockLayoutSettings
{
    int hoverEffect = 2;
    float hoverScale = 1.28f;
    int launchEffect = 1;
    int windowEffect = 1;
};

DockLayoutSettings ApplyDockMonitorOverride(DockLayoutSettings base, const DockMonitorOverride& value);

float ResolveDockScale(const DockLayoutSettings& settings, bool merged);
