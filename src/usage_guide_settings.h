#pragma once

#include "settings_search_index.h"
#include "l10n.h"
#include <array>
#include <optional>

namespace snowdesktop::usage_guide
{
enum class Module { General, Pages, Icons, Beautify, Files, Widgets, Dock, Navigation, Theme, Animation, Taskbar, Backup };
struct IndexModule { Module id; const char* title; const char* advice; const wchar_t* asset; const wchar_t* glyph; };
inline constexpr std::array<IndexModule, 12> kIndexModules{{
    {Module::General, L10N_KEY("app.settings.general"), L10N_KEY("guide.index.general"), L"general.svg", L"\xE713"},
    {Module::Pages, L10N_KEY("settings.nav.pages"), L10N_KEY("guide.index.pages"), L"pages.svg", L"\xE8A9"},
    {Module::Icons, L10N_KEY("app.settings.desktop_icons"), L10N_KEY("guide.index.icons"), L"appearance-desktop-icons.svg", L"\xE8A9"},
    {Module::Beautify, L10N_KEY("app.settings.icon_beautify"), L10N_KEY("guide.index.beautify"), L"appearance-icon-beautification.svg", L"\xE790"},
    {Module::Files, L10N_KEY("settings.nav.categories"), L10N_KEY("guide.index.files"), L"categories.svg", L"\xE8B7"},
    {Module::Widgets, L10N_KEY("app.settings.widgets"), L10N_KEY("guide.index.widgets"), L"widgets.svg", L"\xE74C"},
    {Module::Dock, L10N_KEY("settings.nav.dock"), L10N_KEY("guide.index.dock"), L"dock.svg", L"\xEBC8"},
    {Module::Navigation, L10N_KEY("start.section.navigation"), L10N_KEY("guide.index.navigation"), L"search.svg", L"\xE721"},
    {Module::Theme, L10N_KEY("settings.personalization.theme"), L10N_KEY("guide.index.theme"), L"appearance-theme.svg", L"\xE790"},
    {Module::Animation, L10N_KEY("settings.nav.animation"), L10N_KEY("guide.index.animation"), L"animation-performance.svg", L"\xE768"},
    {Module::Taskbar, L10N_KEY("settings.nav.taskbar"), L10N_KEY("guide.index.taskbar"), L"taskbar.svg", L"\xEBC8"},
    {Module::Backup, L10N_KEY("app.settings.backup"), L10N_KEY("guide.index.backup"), L"backup.svg", L"\xE74E"},
}};

// Classification reuses the visible settings/search catalogue, including its
// feature gates. Hidden entries, guide self-links and diagnostics stay out.
inline std::optional<Module> ClassifySetting(const StaticSettingSearchDescriptor& entry)
{
    if (!entry.visible || entry.focusId.starts_with("start.")) return {};
    const std::string_view id = entry.focusId;
    if (id.starts_with("general.quickNavigation") || id == "personalization.quickNavigationTheme") return Module::Navigation;
    if (id.starts_with("dock.") || id == "personalization.dockAppearance" ||
        id == "animation.hover" || id == "animation.hoverScale" || id == "animation.launch" || id == "animation.window") return Module::Dock;
    if (id == "personalization.collectionPopupTheme") return Module::Widgets;
    switch (entry.page)
    {
    case SettingsPage::General: case SettingsPage::Desktop: return Module::General;
    case SettingsPage::DesktopPages: return Module::Pages;
    case SettingsPage::AppearanceDesktopIcons: return Module::Icons;
    case SettingsPage::AppearanceIconBeautification: return Module::Beautify;
    case SettingsPage::DesktopCategories: return Module::Files;
    case SettingsPage::AppearanceWidgets: case SettingsPage::Widgets: return Module::Widgets;
    case SettingsPage::AppearanceTheme: return Module::Theme;
    case SettingsPage::Dock: case SettingsPage::DockAndTaskbar: return Module::Dock;
    case SettingsPage::Taskbar: return Module::Taskbar;
    case SettingsPage::AnimationPerformance: return Module::Animation;
    case SettingsPage::BackupAndData: return Module::Backup;
    default: return {};
    }
}

// Extra explanations are editorial reference text, never suggested updates.
inline const char* SettingAdvice(std::string_view id)
{
    if (id == "dock.showOnlyWhenSummoned") return L10N_KEY("guide.setting.dockVisibility");
    if (id == "dock.allowDesktopContentOverlap") return L10N_KEY("guide.setting.dockSpace");
    if (id.starts_with("dock.floatingShortcutMode")) return L10N_KEY("guide.setting.dockShortcut");
    if (id == "dock.floatingEdgeSwipe") return L10N_KEY("guide.setting.dockGesture");
    if (id == "dock.floatingEdgeSwipeBlockFullscreen") return L10N_KEY("guide.setting.dockFullscreen");
    if (id == "dock.position") return L10N_KEY("guide.setting.dockPosition");
    if (id == "dock.layout") return L10N_KEY("guide.setting.dockLayout");
    if (id == "dock.monitor") return L10N_KEY("guide.setting.dockMonitor");
    if (id == "dock.thickness") return L10N_KEY("guide.setting.dockSize");
    if (id == "personalization.dockAppearance") return L10N_KEY("guide.setting.dockAppearance");
    if (id == "animation.hover" || id == "animation.hoverScale" || id == "animation.launch" || id == "animation.window")
        return L10N_KEY("guide.setting.dockAnimation");
    if (id == "general.autoStart") return L10N_KEY("guide.setting.startup");
    if (id.starts_with("general.quickNavigation")) return L10N_KEY("guide.setting.navigation");
    return nullptr;
}
}
