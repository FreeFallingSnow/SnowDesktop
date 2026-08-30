#include "settings_route.h"

#include <utility>

namespace snowdesktop
{
namespace
{

bool IsCustomThemeFocus(std::string_view focusId) noexcept
{
    return focusId == "personalization.backgroundColor" ||
        focusId == "personalization.borderColor" ||
        focusId == "personalization.widgetAlpha" ||
        focusId == "personalization.backgroundOpacity" ||
        focusId == "personalization.borderAlpha" ||
        focusId == "personalization.borderOpacity" ||
        focusId == "personalization.borderWidth" ||
        focusId == "personalization.edgeHighlight" ||
        focusId == "personalization.edgeHighlightWidth" ||
        focusId == "personalization.edgeHighlightStrength" ||
        focusId == "personalization.enableGradient" ||
        focusId == "personalization.gradientEndAlpha" ||
        focusId == "personalization.glass" ||
        focusId == "personalization.acrylic" ||
        focusId == "personalization.blurRadius" ||
        focusId == "personalization.contentTheme";
}

bool IsWidgetLayoutFocus(std::string_view focusId) noexcept
{
    return focusId == "personalization.cornerRadius" ||
        focusId == "personalization.barHeight";
}

bool IsDesktopIconAppearanceFocus(std::string_view focusId) noexcept
{
    return focusId == "desktop.spacing" ||
        focusId == "desktop.iconSpacing" ||
        focusId == "desktop.iconSize" ||
        focusId == "desktop.itemFontSize" ||
        focusId == "desktop.listFontSize" ||
        focusId == "desktop.fontWeight" ||
        focusId == "desktop.shortcutArrow";
}

} // namespace

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

SettingsRoute CanonicalizeSettingsRoute(SettingsRoute route)
{
    if (route.page == SettingsPage::General)
    {
        if (route.focusId == "general.pageNavigation" ||
            route.focusId == "general.pageNavigation.previous" ||
            route.focusId == "general.pageNavigation.next")
        {
            route.page = SettingsPage::DesktopPages;
        }
        else if (route.focusId == "general.softwareDesktop")
        {
            route.page = SettingsPage::Desktop;
            route.focusId = "desktop.softwareDesktop";
        }
        else if (route.focusId == "general.doubleClickHide")
        {
            route.page = SettingsPage::Desktop;
            route.focusId = "desktop.doubleClickHide";
        }
        else if (route.focusId == "general.desktopPassthrough" ||
            route.focusId == "general.desktopPassthrough.enabled")
        {
            route.page = SettingsPage::Desktop;
            route.focusId = "desktop.passthrough";
        }
        else if (route.focusId == "general.desktopPassthrough.hotkey")
        {
            route.page = SettingsPage::Desktop;
            route.focusId = "desktop.passthrough.hotkey";
        }
        else if (route.focusId == "general.floatingDock" ||
            route.focusId == "general.floatingDock.enabled")
        {
            route.page = SettingsPage::Dock;
            route.focusId = "dock.floatingShortcutMode";
        }
        else if (route.focusId == "general.floatingDock.hotkey")
        {
            route.page = SettingsPage::Dock;
            route.focusId = "dock.floatingShortcutMode.hotkey";
        }
    }
    else if (route.page == SettingsPage::Personalization)
    {
        if (route.focusId == "personalization.tabHeight" ||
            route.focusId == "desktop.categoryLayout" ||
            route.focusId == "desktop.tabHeight")
        {
            route.page = SettingsPage::AppearanceWidgets;
            route.focusId = "desktop.categoryLayout";
        }
        else if (route.focusId ==
            "personalization.showCategoryTabCounts" ||
            route.focusId == "personalization.showCounts")
        {
            route.page = SettingsPage::DesktopCategories;
            route.focusId = "desktop.categoryCounts";
        }
        else if (IsCustomThemeFocus(route.focusId))
        {
            route.page = SettingsPage::AppearanceTheme;
        }
        else if (IsWidgetLayoutFocus(route.focusId))
        {
            route.page = SettingsPage::AppearanceWidgets;
        }
        else
        {
            // Personalization remains a compatibility input only. Theme,
            // surface-theme, context-menu, empty, and unknown legacy focus
            // identifiers enter the stable first Appearance leaf.
            route.page = SettingsPage::AppearanceTheme;
        }
    }
    else if (route.page == SettingsPage::DockAndTaskbar)
    {
        route.page = route.focusId.starts_with("taskbar.")
            ? SettingsPage::Taskbar
            : SettingsPage::Dock;
    }
    else if (route.page == SettingsPage::Desktop)
    {
        if (route.focusId.starts_with("desktop.iconBeautify"))
        {
            route.page = SettingsPage::AppearanceIconBeautification;
        }
        else if (route.focusId == "desktop.categoryLayout" ||
            route.focusId == "desktop.tabHeight")
        {
            route.page = SettingsPage::AppearanceWidgets;
            route.focusId = "desktop.categoryLayout";
        }
        else if (route.focusId == "desktop.categories" ||
            route.focusId.starts_with("desktop.categoryRules") ||
            route.focusId == "desktop.category.add" ||
            route.focusId == "desktop.categoryCounts")
        {
            route.page = SettingsPage::DesktopCategories;
        }
        else if (IsDesktopIconAppearanceFocus(route.focusId))
        {
            route.page = SettingsPage::AppearanceDesktopIcons;
        }
    }
    else if (route.page == SettingsPage::DesktopCategories &&
        (route.focusId == "desktop.categoryLayout" ||
            route.focusId == "desktop.tabHeight"))
    {
        route.page = SettingsPage::AppearanceWidgets;
        route.focusId = "desktop.categoryLayout";
    }
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
    case SettingsPage::Dock:
    case SettingsPage::Taskbar:
    case SettingsPage::DesktopCategories:
    case SettingsPage::AppearanceTheme:
    case SettingsPage::AppearanceWidgets:
    case SettingsPage::AppearanceDesktopIcons:
    case SettingsPage::AppearanceIconBeautification:
    case SettingsPage::DesktopPages:
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
    case SettingsPage::Dock: return "dock";
    case SettingsPage::Taskbar: return "taskbar";
    case SettingsPage::DesktopCategories: return "desktop-categories";
    case SettingsPage::AppearanceTheme: return "appearance-theme";
    case SettingsPage::AppearanceWidgets: return "appearance-widgets";
    case SettingsPage::AppearanceDesktopIcons:
        return "appearance-desktop-icons";
    case SettingsPage::AppearanceIconBeautification:
        return "appearance-icon-beautification";
    case SettingsPage::DesktopPages: return "desktop-pages";
    }
    return "home";
}

} // namespace snowdesktop
