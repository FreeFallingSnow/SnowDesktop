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

struct StatusBarSettings
{
    bool enabled = false;
    DockPosition position = DockPosition::Top;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    bool showOnlyWhenSummoned = false;
    int edgeRevealGesture = 1; // 0=swipe, 1=hover; independent bars default to hover.
    float scale = 1.0f;
    SurfaceTheme theme;
    StatusBarAppearanceRule noWindow, maximizedWindow;
    // Retired scenes are read and round-tripped under their original JSON keys
    // only. Never reinterpret "has visible windows" as "has no windows".
    StatusBarAppearanceRule legacyShellUi, legacyVisibleWindow;
    bool menu = true, quickSearch = true, taskView = true;
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
    friend bool operator==(const StatusBarSettings&, const StatusBarSettings&) = default;
};

}
