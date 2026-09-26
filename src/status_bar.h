#pragma once
#include "status_bar_appearance.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

struct IDCompositionDesktopDevice;
struct IDWriteFactory;
struct ID2D1DeviceContext;
namespace snowdesktop
{
namespace widget_runtime { class WidgetSystemDataProvider; }
namespace tray { class Service; }
enum class StatusBarAction { Calendar, Tray, Network, Audio, Power, ControlCenter, Settings, Menu, QuickSearch, SystemMenu, None, Notifications, Cpu, Memory, Gpu, Traffic, Dismiss, SystemControlCenter };
struct StatusBarMonitor
{
    std::wstring id;
    HMONITOR monitor = nullptr;
    int mergedDockHeight = 0; // Physical pixels; zero means separate.
};
class StatusBar final
{
public:
    using Activate = std::function<void(StatusBarAction, HWND, RECT)>;
    using Hidden = std::function<void(HMONITOR)>;
    using Error = std::function<void(const std::wstring&)>;
    using DrawBackground = std::function<void(ID2D1DeviceContext*, RECT,
        const PersonalizationSettings&, float)>;
    StatusBar(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data,
        Activate activate, Hidden hidden, Error error, DrawBackground drawBackground);
    ~StatusBar();
    StatusBar(const StatusBar&) = delete;
    StatusBar& operator=(const StatusBar&) = delete;
    // UI-thread only. Monitor order is resolved by the application's page roles.
    void Configure(StatusBarSettings settings, const PersonalizationSettings& global,
        const std::vector<StatusBarMonitor>& monitors,
        IDCompositionDesktopDevice* composition, IDWriteFactory* text,
        const PersonalizationSettings* tooltipAppearance = nullptr,
        DrawBackground drawTooltipBackground = {});
    void Close();
    // Release a lost device without unregistering the AppBar or sampling demands.
    void ReleaseGraphicsResources();
    bool IsFullscreen(HMONITOR monitor) const;
    std::shared_ptr<tray::Service> Tray() const;
    void SetTrayDragHandlers(std::function<void(const StatusBarSettings&)> changed,
        std::function<bool(std::string_view, POINT)> dropOutside);
    bool DropTrayIcon(std::string_view key, POINT screen);
    std::optional<RECT> MergedDockArea(HMONITOR monitor) const;
    void SetDockChanged(std::function<void(bool geometry)> changed);
    // Application-owned window observations; must not enable taskbar effects.
    void SetSceneProvider(std::function<StatusBarSceneState(HMONITOR)> provider);
    PersonalizationSettings AppearanceForMonitor(HMONITOR monitor) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
