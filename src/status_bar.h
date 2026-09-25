#pragma once
#include "status_bar_settings.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct IDCompositionDesktopDevice;
struct IDWriteFactory;
struct ID2D1DeviceContext;
namespace snowdesktop
{
namespace widget_runtime { class WidgetSystemDataProvider; }
enum class StatusBarAction { Calendar, Tray, Network, Audio, Power, ControlCenter, Settings };
struct StatusBarMonitor
{
    std::wstring id;
    HMONITOR monitor = nullptr;
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
        IDCompositionDesktopDevice* composition, IDWriteFactory* text);
    void Close();
    bool IsFullscreen(HMONITOR monitor) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
