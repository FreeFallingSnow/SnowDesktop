#include "settings_route.h"

#include <utility>

namespace snowdesktop
{

SettingsRoute SettingsRoute::ForPage(
    SettingsPage page,
    std::string focusId)
{
    SettingsRoute route;
    route.page = page;
    route.focusId = std::move(focusId);
    return route;
}

SettingsRoute SettingsRoute::ForWidget(
    std::wstring instanceId,
    std::string focusId)
{
    SettingsRoute route;
    route.page = SettingsPage::WidgetSettings;
    route.widgetInstanceId = std::move(instanceId);
    route.focusId = std::move(focusId);
    return route;
}

bool SettingsRoute::IsValid() const noexcept
{
    switch (page)
    {
    case SettingsPage::Home:
    case SettingsPage::General:
    case SettingsPage::Personalization:
    case SettingsPage::Desktop:
    case SettingsPage::DockAndTaskbar:
    case SettingsPage::Widgets:
    case SettingsPage::WidgetSettings:
    case SettingsPage::BackupAndData:
    case SettingsPage::About:
    case SettingsPage::DeveloperTools:
    case SettingsPage::Debug:
        break;
    default:
        return false;
    }

    if (page == SettingsPage::WidgetSettings)
        return !widgetInstanceId.empty();
    return widgetInstanceId.empty();
}

std::string_view SettingsPageKey(SettingsPage page) noexcept
{
    switch (page)
    {
    case SettingsPage::Home: return "home";
    case SettingsPage::General: return "general";
    case SettingsPage::Personalization: return "personalization";
    case SettingsPage::Desktop: return "desktop";
    case SettingsPage::DockAndTaskbar: return "dock-and-taskbar";
    case SettingsPage::Widgets: return "widgets";
    case SettingsPage::WidgetSettings: return "widget-settings";
    case SettingsPage::BackupAndData: return "backup-and-data";
    case SettingsPage::About: return "about";
    case SettingsPage::DeveloperTools: return "developer-tools";
    case SettingsPage::Debug: return "debug";
    }
    return "home";
}

} // namespace snowdesktop
