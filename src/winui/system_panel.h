#pragma once
#include "../status_bar.h"
#include "system_calendar_view.h"
#include "../ui_animation_scheduler.h"
#include <memory>

namespace snowdesktop::winui
{
// One island and one popup for all status bar surfaces. Reused for the control
// center, tray and calendar so they cannot overlap or create competing runtimes.
class SystemPanel
{
public:
    using SettingsChanged = std::function<void(const StatusBarSettings&)>;
    explicit SystemPanel(SettingsChanged changed, SystemCalendarActions calendar = {},
        std::function<bool(std::string_view, POINT)> dropOutside = {}, UiAnimationScheduler* scheduler = nullptr);
    ~SystemPanel();
    void Show(StatusBarAction action, HWND owner, RECT anchor,
        const PersonalizationSettings& appearance, const StatusBarSettings& settings,
        std::shared_ptr<tray::Service> tray,
        std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data);
    void Hide();
    void HideForMonitor(HMONITOR monitor);
    bool PreTranslateMessage(MSG* message);
    bool DropTrayIcon(std::string_view key, POINT screen);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
