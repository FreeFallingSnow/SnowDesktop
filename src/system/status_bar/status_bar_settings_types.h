#pragma once
#include "dock/dock_layout_settings.h"
#include "theme/surface_theme_types.h"
#include <string>
#include <vector>

namespace snowdesktop
{
struct StatusBarAppearanceRule
{
    bool enabled = false;
    SurfaceTheme theme = [] {
        SurfaceTheme value;
        // Panel appearance persistence stores material fields, not a preset ID.
        value.appearance.backgroundPreset = kAppearancePresetCustom;
        return value;
    }();
    friend bool operator==(const StatusBarAppearanceRule&, const StatusBarAppearanceRule&) = default;
};

struct StatusBarMonitorOverride
{
    bool operator==(const StatusBarMonitorOverride&) const = default;
    std::optional<DockPosition> position;
    std::optional<float> scale;
    std::optional<bool> showOnlyWhenSummoned;
    std::optional<int> edgeRevealGesture;

    template <typename Self, typename Visitor>
    static void Visit(Self& value, Visitor&& visit)
    {
        visit("position", value.position);
        visit("scale", value.scale);
        visit("showOnlyWhenSummoned", value.showOnlyWhenSummoned);
        visit("edgeRevealGesture", value.edgeRevealGesture);
    }
};

struct StatusBarMonitorLayout
{
    DockPosition position = DockPosition::Top;
    float scale = 1.f;
    bool showOnlyWhenSummoned = false;
    int edgeRevealGesture = 1;
    bool operator==(const StatusBarMonitorLayout&) const = default;
};

struct StatusBarSettings
{
    bool enabled = false;
    DockPosition position = DockPosition::Top;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    bool showOnlyWhenSummoned = false;
    int edgeRevealGesture = 1; // 0=swipe, 1=hover; independent bars default to hover.
    float scale = 1.0f;
    StatusBarMonitorOverride lastMonitorOverride, otherMonitorOverride;
    int monitorVisibility = kDefaultBarMonitorVisibility;
    SurfaceTheme theme;
    StatusBarAppearanceRule appWindow, maximizedWindow;
    // Retired scenes round-trip under their original JSON keys.
    StatusBarAppearanceRule noWindow;
    StatusBarAppearanceRule legacyShellUi, legacyVisibleWindow;
    bool menu = true, quickSearch = true, taskView = true;
    bool foregroundAppName = false;
    bool foregroundAppNameCentered = false;
    bool showDesktop = true;
    bool clockSystemPanel = false, controlCenterSystemPanel = false;
    bool clockShowSeconds = false;
    bool timeNotificationRightAligned = false;
    bool clock = true, tray = true, network = true, volume = true, battery = true;
    bool controlCenter = true;
    bool controlCenterSingleIcon = false;
    bool notifications = true;
    bool batteryPercentage = false;
    bool inputMethod = true;
    bool inputMethodSystemPanel = false;
    bool cpu = false, memory = false, gpu = false, traffic = false;
    bool audioControls = true, brightnessControls = true, wifiControls = true;
    bool bluetoothControls = true, mediaControls = true, powerControls = true;
    std::vector<std::string> pinnedTrayItems;
    std::vector<std::string> trayOrder;
    // Only the controls the user has opted into. Controls missing from this
    // list stay hidden; the control-center visibility editor appends them.
    std::vector<std::string> quickControlOrder = {"projection", "hotspot", "airplane", "microphone", "awake", "power"};
    std::vector<std::string> hiddenQuickControls;
    std::vector<std::string> leftOrder = {"menu", "quickSearch", "taskView"};
    std::vector<std::string> rightOrder = {"tray", "cpu", "memory", "gpu", "traffic", "network", "volume", "battery", "controlCenter"};
    std::vector<std::string> metricOrder = {"cpu", "memory", "gpu", "traffic"};
    friend bool operator==(const StatusBarSettings&, const StatusBarSettings&) = default;

    StatusBarMonitorLayout MonitorLayout(BarMonitorRole role) const
    {
        const StatusBarMonitorLayout base{position, scale, showOnlyWhenSummoned, edgeRevealGesture};
        if (role == BarMonitorRole::First) return base;
        const auto& value = role == BarMonitorRole::Last ? lastMonitorOverride : otherMonitorOverride;
        return {value.position.value_or(position), value.scale.value_or(scale),
            value.showOnlyWhenSummoned.value_or(showOnlyWhenSummoned),
            value.edgeRevealGesture.value_or(edgeRevealGesture)};
    }
};

}
