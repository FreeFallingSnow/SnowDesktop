#pragma once
#include "common/l10n.h"
#include "settings/settings_route.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// Private reference catalogue; only the outer expansion preference is saved.
namespace snowdesktop::usage_guide
{
enum class Section { Layout, Files, Pages, Daily, Appearance, Extensions };
enum class Topic
{
    Startup,
    Grid,
    Icons,
    Beautify,
    Theme,
    Collection,
    Application,
    Move,
    Resize,
    CollectionGroup,
    Files,
    FolderMapping,
    FileGroup,
    DockPin,
    DockMapping,
    DockCollection,
    DockFiles,
    DockSummon,
    Navigation,
    LuaWidget,
    Backup,
    CategoryRules,
    DockEnable,
    DockPosition,
    DockHotkeys,
    DockSpace,
    DockAppearance,
    Workshop,
    Develop,
    PagesOverview,
    PagesManage,
    PagesEdge,
    PagesDrag,
    PagesKeys,
    WidgetLayout,
    ContextMenu,
    DesktopStyle,
    Taskbar,
    StatusBarEnable,
    StatusBarContents,
    WidgetPreferences,
    Calendar,
    Animation
};
struct Lesson
{
    Topic topic;
    const char* key;
    Section section;
    const char* title;
    const char* description;
    const char* instructions;
    SettingsPage settingsPage;
    const char* settingsFocus;
    bool practice;
    bool needsDock = false;
    SettingsPage secondaryPage = SettingsPage::General;
    const char* secondaryFocus = "";
    const char* secondaryLabel = "";
};
inline constexpr std::array<Lesson, 40> kLessons{{
    {Topic::DesktopStyle, "desktopStyle", Section::Layout, L10N_KEY("start.desktopStyle.title"),
     L10N_KEY("start.desktopStyle.description"), L10N_KEY("start.desktopStyle.instructions"),
     SettingsPage::DesktopStyle, "desktopStyle", false},
    {Topic::DockEnable, "dockEnable", Section::Layout, L10N_KEY("start.dockEnable.title"),
     L10N_KEY("start.dockEnable.description"), L10N_KEY("start.dockEnable.instructions"), SettingsPage::Dock,
     "dock.enable", false, false},
    {Topic::DockPosition, "dockPosition", Section::Layout, L10N_KEY("start.dockPosition.title"),
     L10N_KEY("start.dockPosition.description"), L10N_KEY("start.dockPosition.instructions"),
     SettingsPage::Dock, "dock.position", false, false},
    {Topic::StatusBarEnable, "statusBarEnable", Section::Layout, L10N_KEY("start.statusBarEnable.title"),
     L10N_KEY("start.statusBarEnable.description"), L10N_KEY("start.statusBarEnable.instructions"),
     SettingsPage::StatusBar, "statusBar.enable", false},
    {Topic::Taskbar, "taskbar", Section::Layout, L10N_KEY("start.taskbar.title"),
     L10N_KEY("start.taskbar.description"), L10N_KEY("start.taskbar.instructions"),
     SettingsPage::Taskbar, "taskbar.displayMode", false},
    {Topic::DockSpace, "dockSpace", Section::Layout, L10N_KEY("start.dockSpace.title"),
     L10N_KEY("start.dockSpace.description"), L10N_KEY("start.dockSpace.instructions"), SettingsPage::Dock,
     "dock.allowDesktopContentOverlap", false, false},
    {Topic::Move, "move", Section::Files, L10N_KEY("start.move.title"), L10N_KEY("start.move.description"),
     L10N_KEY("start.move.instructions"), SettingsPage::General, "", true, false},
    {Topic::Collection, "collection", Section::Files, L10N_KEY("start.collection.title"),
     L10N_KEY("start.collection.description"), L10N_KEY("start.collection.instructions"),
     SettingsPage::General, "", true, false},
    {Topic::Files, "files", Section::Files, L10N_KEY("start.files.title"),
     L10N_KEY("start.files.description"), L10N_KEY("start.files.instructions"), SettingsPage::General, "",
     true, false},
    {Topic::FolderMapping, "folderMapping", Section::Files, L10N_KEY("start.folderMapping.title"),
     L10N_KEY("start.folderMapping.description"), L10N_KEY("start.folderMapping.instructions"),
     SettingsPage::General, "", true, false},
    {Topic::CollectionGroup, "collectionGroup", Section::Files, L10N_KEY("start.collectionGroup.title"),
     L10N_KEY("start.collectionGroup.description"), L10N_KEY("start.collectionGroup.instructions"),
     SettingsPage::General, "", true, false},
    {Topic::FileGroup, "fileGroup", Section::Files, L10N_KEY("start.fileGroup.title"),
     L10N_KEY("start.fileGroup.description"), L10N_KEY("start.fileGroup.instructions"), SettingsPage::General,
     "", true, false},
    {Topic::CategoryRules, "categoryRules", Section::Files, L10N_KEY("start.categoryRules.title"),
     L10N_KEY("start.categoryRules.description"), L10N_KEY("start.categoryRules.instructions"),
     SettingsPage::DesktopCategories, "desktop.categoryRules", false, false},
    {Topic::LuaWidget, "luaWidget", Section::Files, L10N_KEY("start.luaWidget.title"),
     L10N_KEY("start.luaWidget.description"), L10N_KEY("start.luaWidget.instructions"), SettingsPage::Widgets,
     "widgets.included", true, false},
    {Topic::PagesOverview, "pagesOverview", Section::Pages, L10N_KEY("start.pagesOverview.title"),
     L10N_KEY("start.pagesOverview.description"), L10N_KEY("start.pagesOverview.instructions"),
     SettingsPage::DesktopPages, "pages.order", false},
    {Topic::PagesManage, "pagesManage", Section::Pages, L10N_KEY("start.pagesManage.title"),
     L10N_KEY("start.pagesManage.description"), L10N_KEY("start.pagesManage.instructions"),
     SettingsPage::DesktopPages, "pages.order", true},
    {Topic::Grid, "grid", Section::Pages, L10N_KEY("start.grid.title"), L10N_KEY("start.grid.description"),
     L10N_KEY("start.grid.instructions"), SettingsPage::DesktopPages, "pages.grid", false, false},
    {Topic::PagesEdge, "pagesEdge", Section::Pages, L10N_KEY("start.pagesEdge.title"),
     L10N_KEY("start.pagesEdge.description"), L10N_KEY("start.pagesEdge.instructions"),
     SettingsPage::DesktopPages, "pages.add", true},
    {Topic::PagesDrag, "pagesDrag", Section::Pages, L10N_KEY("start.pagesDrag.title"),
     L10N_KEY("start.pagesDrag.description"), L10N_KEY("start.pagesDrag.instructions"),
     SettingsPage::DesktopPages, "pages.add", true},
    {Topic::PagesKeys, "pagesKeys", Section::Pages, L10N_KEY("start.pagesKeys.title"),
     L10N_KEY("start.pagesKeys.description"), L10N_KEY("start.pagesKeys.instructions"),
     SettingsPage::DesktopPages, "general.pageNavigation", false},
    {Topic::Startup, "startup", Section::Daily, L10N_KEY("start.startup.title"),
     L10N_KEY("start.startup.description"), L10N_KEY("start.startup.instructions"), SettingsPage::General,
     "general.autoStart", false, false},
    {Topic::Navigation, "navigation", Section::Daily, L10N_KEY("start.navigation.title"),
     L10N_KEY("start.navigation.description"), L10N_KEY("start.navigation.instructions"),
     SettingsPage::QuickNavigation, "general.quickNavigation", false, false},
    {Topic::DockPin, "dockPin", Section::Daily, L10N_KEY("start.dockPin.title"),
     L10N_KEY("start.dockPin.description"), L10N_KEY("start.dockPin.instructions"), SettingsPage::Dock,
     "dock.enable", true, true},
    {Topic::DockCollection, "dockCollection", Section::Daily, L10N_KEY("start.dockCollection.title"),
     L10N_KEY("start.dockCollection.description"), L10N_KEY("start.dockCollection.instructions"),
     SettingsPage::Dock, "dock.enable", true, true},
    {Topic::DockFiles, "dockFiles", Section::Daily, L10N_KEY("start.dockFiles.title"),
     L10N_KEY("start.dockFiles.description"), L10N_KEY("start.dockFiles.instructions"), SettingsPage::Dock,
     "dock.enable", true, true},
    {Topic::DockSummon, "dockSummon", Section::Daily, L10N_KEY("start.dockSummon.title"),
     L10N_KEY("start.dockSummon.description"), L10N_KEY("start.dockSummon.instructions"), SettingsPage::Dock,
     "dock.showOnlyWhenSummoned", false, false},
    {Topic::DockHotkeys, "dockHotkeys", Section::Daily, L10N_KEY("start.dockHotkeys.title"),
     L10N_KEY("start.dockHotkeys.description"), L10N_KEY("start.dockHotkeys.instructions"),
     SettingsPage::Dock, "dock.floatingShortcutMode", false, false},
    {Topic::StatusBarContents, "statusBarContents", Section::Daily, L10N_KEY("start.statusBarContents.title"),
     L10N_KEY("start.statusBarContents.description"), L10N_KEY("start.statusBarContents.instructions"),
     SettingsPage::StatusBar, "statusBar.menu", false},
    {Topic::Calendar, "calendar", Section::Daily, L10N_KEY("start.calendar.title"),
     L10N_KEY("start.calendar.description"), L10N_KEY("start.calendar.instructions"),
     SettingsPage::Calendar, "calendar.events", false},
    {Topic::ContextMenu, "contextMenu", Section::Daily, L10N_KEY("start.contextMenu.title"),
     L10N_KEY("start.contextMenu.description"), L10N_KEY("start.contextMenu.instructions"),
     SettingsPage::ContextMenu, "contextMenu.extensions", false},
    {Topic::Theme, "theme", Section::Appearance, L10N_KEY("start.theme.title"),
     L10N_KEY("start.theme.description"), L10N_KEY("start.theme.instructions"), SettingsPage::AppearanceTheme,
     "personalization.theme", false, false},
    {Topic::Icons, "icons", Section::Appearance, L10N_KEY("start.icons.title"),
     L10N_KEY("start.icons.description"), L10N_KEY("start.icons.instructions"),
     SettingsPage::AppearanceDesktopIcons, "desktop.iconSize", false, false},
    {Topic::Beautify, "beautify", Section::Appearance, L10N_KEY("start.beautify.title"),
     L10N_KEY("start.beautify.description"), L10N_KEY("start.beautify.instructions"),
     SettingsPage::AppearanceIconBeautification, "desktop.iconBeautify", false, false},
    {Topic::WidgetLayout, "widgetLayout", Section::Appearance, L10N_KEY("start.widgetLayout.title"),
     L10N_KEY("start.widgetLayout.description"), L10N_KEY("start.widgetLayout.instructions"),
     SettingsPage::AppearanceDesktopIcons, "personalization.cornerRadius", false, false},
    {Topic::DockAppearance, "dockAppearance", Section::Appearance, L10N_KEY("start.dockAppearance.title"),
     L10N_KEY("start.dockAppearance.description"), L10N_KEY("start.dockAppearance.instructions"),
     SettingsPage::AppearanceTheme, "personalization.dockAppearance", false, false,
     SettingsPage::AnimationPerformance, "animation.hover", L10N_KEY("settings.nav.animation")},
    {Topic::WidgetPreferences, "widgetPreferences", Section::Appearance,
     L10N_KEY("start.widgetPreferences.title"), L10N_KEY("start.widgetPreferences.description"),
     L10N_KEY("start.widgetPreferences.instructions"), SettingsPage::WidgetBehavior,
     "personalization.groupTabHoverSwitch", false},
    {Topic::Animation, "animation", Section::Appearance, L10N_KEY("start.animation.title"),
     L10N_KEY("start.animation.description"), L10N_KEY("start.animation.instructions"),
     SettingsPage::AnimationPerformance, "animation.mode", false},
    {Topic::Backup, "backup", Section::Extensions, L10N_KEY("start.backup.title"),
     L10N_KEY("start.backup.description"), L10N_KEY("start.backup.instructions"), SettingsPage::BackupAndData,
     "backup.layout", false, false},
    {Topic::Workshop, "workshop", Section::Extensions, L10N_KEY("start.workshop.title"),
     L10N_KEY("start.workshop.description"), L10N_KEY("start.workshop.instructions"), SettingsPage::Widgets,
     "widgets.workshop", false, false},
    {Topic::Develop, "develop", Section::Extensions, L10N_KEY("start.develop.title"),
     L10N_KEY("start.develop.description"), L10N_KEY("start.develop.instructions"),
     SettingsPage::DeveloperTools, "developer.agentSkill", false, false},
}};
inline const Lesson* Find(Topic topic)
{
    for (const auto& lesson : kLessons) if (lesson.topic == topic) return &lesson;
    return nullptr;
}
inline std::optional<Topic> ParseTopic(std::string_view key)
{
    if (key == "application") return Topic::Collection;
    if (key == "layout" || key == "resize") return Topic::Move;
    if (key == "dockMapping") return Topic::DockPin;
    for (const auto& lesson : kLessons) if (key == lesson.key) return lesson.topic;
    return std::nullopt;
}
inline const char* SectionTitle(Section section)
{
    switch (section)
    {
    case Section::Layout: return L10N_KEY("start.basics");
    case Section::Files: return L10N_KEY("start.section.files");
    case Section::Daily: return L10N_KEY("start.section.dock");
    case Section::Appearance: return L10N_KEY("start.section.appearance");
    case Section::Extensions: return L10N_KEY("start.section.lua");
    case Section::Pages: return L10N_KEY("start.section.pages");
    }
    return L10N_KEY("start.title");
}
inline const char* SectionDescription(Section section)
{
    switch (section)
    {
    case Section::Layout: return L10N_KEY("start.section.layout.description");
    case Section::Files: return L10N_KEY("start.section.files.description");
    case Section::Pages: return L10N_KEY("start.section.pages.description");
    case Section::Daily: return L10N_KEY("start.section.daily.description");
    case Section::Appearance: return L10N_KEY("start.section.appearance.description");
    case Section::Extensions: return L10N_KEY("start.section.extensions.description");
    }
    return L10N_KEY("start.description");
}
inline bool HasSettings(const Lesson& lesson)
{
    return !lesson.practice || lesson.settingsPage != SettingsPage::General || *lesson.settingsFocus;
}
// Route choices describe existing preferences; they never apply a setting.
inline bool CanShowDesktop(const Lesson& lesson, bool dockEnabled)
{ return lesson.practice && (!lesson.needsDock || dockEnabled); }
inline SettingsRoute SettingsDestination(const Lesson& lesson, bool secondary = false)
{
    auto route = SettingsRoute::ForPage(secondary ? lesson.secondaryPage : lesson.settingsPage,
        secondary ? lesson.secondaryFocus : lesson.settingsFocus);
    route.guideTopic = lesson.key;
    return route;
}
inline std::optional<SettingsRoute> ReturnDestination(const SettingsRoute& route)
{
    const auto topic = ParseTopic(route.guideTopic);
    if (!topic) return std::nullopt;
    return SettingsRoute::ForPage(SettingsPage::General, "start." + std::string(Find(*topic)->key));
}
bool LoadExpanded(const std::filesystem::path& path, bool& expanded, std::string* error = nullptr);
bool SaveExpanded(const std::filesystem::path& path, bool expanded, std::string* error = nullptr);
}
