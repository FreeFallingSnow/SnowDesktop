#pragma once

#include "settings_route.h"

namespace snowdesktop
{
// Presentation metadata shared by navigation, headings and offline search.
// Existing SettingsPage values and serialized route keys remain unchanged.
enum class SettingsCategory { None, DesktopOrganization, DesktopLayout, Appearance };

constexpr SettingsCategory SettingsPageCategory(SettingsPage page) noexcept
{
    switch (page)
    {
    case SettingsPage::Desktop: case SettingsPage::QuickNavigation:
    case SettingsPage::DesktopPages: case SettingsPage::WidgetBehavior:
    case SettingsPage::DesktopCategories:
        return SettingsCategory::DesktopOrganization;
    case SettingsPage::DesktopStyle: case SettingsPage::Dock:
    case SettingsPage::DockAndTaskbar: case SettingsPage::StatusBar: case SettingsPage::Taskbar:
        return SettingsCategory::DesktopLayout;
    case SettingsPage::Personalization: case SettingsPage::AppearanceTheme:
    case SettingsPage::AppearanceWidgets: case SettingsPage::AppearanceDesktopIcons:
    case SettingsPage::AppearanceIconBeautification: case SettingsPage::LargeIcon:
    case SettingsPage::AnimationPerformance: case SettingsPage::ContextMenu:
    case SettingsPage::ThemeManager:
    case SettingsPage::IconLibrary:
        return SettingsCategory::Appearance;
    default: return SettingsCategory::None;
    }
}

constexpr std::string_view SettingsCategoryTitleKey(SettingsCategory category) noexcept
{
    switch (category)
    {
    case SettingsCategory::DesktopOrganization: return "settings.nav.group.organization";
    case SettingsCategory::DesktopLayout: return "settings.nav.group.layout";
    case SettingsCategory::Appearance: return "settings.nav.group.appearanceInteraction";
    default: return {};
    }
}

constexpr std::string_view SettingsPageTitleKey(SettingsPage page) noexcept
{
    switch (page)
    {
    case SettingsPage::Home: case SettingsPage::General: return "app.settings.general";
    case SettingsPage::QuickNavigation: return "quickNav.title";
    case SettingsPage::Personalization: case SettingsPage::AppearanceTheme:
    case SettingsPage::AppearanceWidgets: return "settings.nav.themeMaterials";
    case SettingsPage::ThemeManager: return "themeLibrary.manager";
    case SettingsPage::IconLibrary: return "iconLibrary.title";
    case SettingsPage::AppearanceDesktopIcons: return "settings.nav.fontsIcons";
    case SettingsPage::AppearanceIconBeautification: return "app.settings.icon_beautify";
    case SettingsPage::WidgetBehavior: return "settings.widgetBehavior.title";
    case SettingsPage::Desktop: return "settings.nav.desktop";
    case SettingsPage::DesktopPages: return "settings.nav.pages";
    case SettingsPage::DesktopCategories: return "settings.nav.categories";
    case SettingsPage::AnimationPerformance: return "settings.nav.animation";
    case SettingsPage::ContextMenu: return "settings.contextMenu.page";
    case SettingsPage::Calendar: return "settings.calendar.page";
    case SettingsPage::DesktopStyle: return "settings.desktopStyle.title";
    case SettingsPage::Dock: case SettingsPage::DockAndTaskbar: return "settings.nav.dock";
    case SettingsPage::StatusBar: return "settings.nav.statusBar";
    case SettingsPage::Taskbar: return "settings.nav.taskbar";
    case SettingsPage::Widgets: return "settings.nav.widgetManagement";
    case SettingsPage::WidgetSettings: return "app.settings.widgets";
    case SettingsPage::LargeIcon: return "largeIcon.settings";
    case SettingsPage::BackupAndData: return "settings.nav.backupData";
    case SettingsPage::About: return "app.settings.about";
    case SettingsPage::DeveloperTools: return "app.settings.widgets_developer_tools";
    case SettingsPage::Debug: return "app.settings.debug";
    default: return {};
    }
}
} // namespace snowdesktop
