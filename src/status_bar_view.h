#pragma once
#include "status_bar.h"
#include "tray_service.h"
#include "widget_system_data_provider.h"
#include <d2d1_1.h>
#include <optional>
#include <array>

namespace snowdesktop
{
struct StatusBarItem
{
    std::wstring text;
    StatusBarAction action;
    RECT bounds{};
    std::optional<tray::Icon> icon;
    std::string key;
    std::wstring glyph, tip;
    bool left = false;
    std::array<std::wstring, 3> controlTips;
};

// Data boundary only: no HWND, sampling, Explorer or device operations.
struct StatusBarSnapshot
{
    std::wstring clock;
    std::optional<widget_runtime::WidgetCpuDataSnapshot> cpu;
    std::optional<widget_runtime::WidgetMemoryDataSnapshot> memory;
    std::optional<widget_runtime::WidgetGpuDataSnapshot> gpu;
    std::optional<widget_runtime::WidgetNetworkTrafficDataSnapshot> traffic;
    std::optional<widget_runtime::WidgetNetworkStatusDataSnapshot> network;
    std::optional<widget_runtime::WidgetAudioOutputVolumeDataSnapshot> audio;
    std::optional<widget_runtime::WidgetPowerDataSnapshot> power;
    std::vector<tray::Icon> tray;
};
struct StatusBarPalette
{
    bool highContrast = false;
    D2D1_COLOR_F background{}, foreground{}, highlight{}, highlightText{};
};
std::vector<StatusBarItem> BuildStatusBarItems(const StatusBarSettings&, const StatusBarSnapshot&);
bool SameStatusBarContent(const std::vector<StatusBarItem>&, const std::vector<StatusBarItem>&);
inline std::size_t StatusBarControlPart(float localDip)
{ return localDip < 32 ? 0 : localDip < 60 ? 1 : 2; }
inline std::optional<std::size_t> HitTestStatusBarItems(const std::vector<StatusBarItem>& items, POINT point)
{
    for (std::size_t index = 0; index < items.size(); ++index)
        if (PtInRect(&items[index].bounds, point)) return index;
    return {};
}
HRESULT DrawStatusBarContent(ID2D1DeviceContext*, IDWriteFactory*, std::vector<StatusBarItem>&,
    UINT width, UINT height, float scale, const PersonalizationSettings&, const StatusBarPalette&,
    std::optional<std::size_t> hovered = {}, bool keyboardFocusVisible = false, std::size_t focused = 0,
    bool mergedDock = false);
PersonalizationSettings StatusBarFillAppearance(const PersonalizationSettings&);
void DrawStatusBarEdge(ID2D1DeviceContext*, RECT, const PersonalizationSettings&, float scale, DockPosition);
}
