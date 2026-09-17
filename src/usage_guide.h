#pragma once
#include "l10n.h"
#include "settings_route.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// Private reference catalogue; only the outer expansion preference is saved.
namespace snowdesktop::usage_guide
{
enum class Section { Basics, Files, Dock, More, Pages };
enum class Topic { Startup, Grid, Icons, Beautify, Theme, Collection, Application, Move, Resize, CollectionGroup, Files, FolderMapping, FileGroup, DockPin, DockMapping, DockCollection, DockFiles, DockSummon, Navigation, LuaWidget, Backup, CategoryRules, DockEnable, DockPosition, DockHotkeys, DockSpace, DockAppearance, Workshop, Develop, PagesOverview, PagesManage, PagesEdge, PagesDrag, PagesKeys };
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
inline constexpr std::array<Lesson, 31> kLessons{{
    {Topic::Startup, "startup", Section::Basics, L10N_KEY("start.startup.title"), L10N_KEY("start.startup.description"), L10N_KEY("start.startup.instructions"), SettingsPage::General, "general.autoStart", false, false},
    {Topic::Move, "move", Section::Basics, L10N_KEY("start.move.title"), L10N_KEY("start.move.description"), L10N_KEY("start.move.instructions"), SettingsPage::General, "", true, false},
    {Topic::Grid, "grid", Section::Basics, L10N_KEY("start.grid.title"), L10N_KEY("start.grid.description"), L10N_KEY("start.grid.instructions"), SettingsPage::DesktopPages, "pages.grid", false, false},
    {Topic::Icons, "icons", Section::Basics, L10N_KEY("start.icons.title"), L10N_KEY("start.icons.description"), L10N_KEY("start.icons.instructions"), SettingsPage::AppearanceDesktopIcons, "desktop.iconSize", false, false},
    {Topic::Beautify, "beautify", Section::Basics, L10N_KEY("start.beautify.title"), L10N_KEY("start.beautify.description"), L10N_KEY("start.beautify.instructions"), SettingsPage::AppearanceIconBeautification, "desktop.iconBeautify", false, false},
    {Topic::Theme, "theme", Section::Basics, L10N_KEY("start.theme.title"), L10N_KEY("start.theme.description"), L10N_KEY("start.theme.instructions"), SettingsPage::AppearanceTheme, "personalization.theme", false, false},
    {Topic::Backup, "backup", Section::Basics, L10N_KEY("start.backup.title"), L10N_KEY("start.backup.description"), L10N_KEY("start.backup.instructions"), SettingsPage::BackupAndData, "backup.layout", false, false},
    {Topic::Collection, "collection", Section::Files, L10N_KEY("start.collection.title"), L10N_KEY("start.collection.description"), L10N_KEY("start.collection.instructions"), SettingsPage::General, "", true, false},
    {Topic::CollectionGroup, "collectionGroup", Section::Files, L10N_KEY("start.collectionGroup.title"), L10N_KEY("start.collectionGroup.description"), L10N_KEY("start.collectionGroup.instructions"), SettingsPage::General, "", true, false},
    {Topic::Files, "files", Section::Files, L10N_KEY("start.files.title"), L10N_KEY("start.files.description"), L10N_KEY("start.files.instructions"), SettingsPage::General, "", true, false},
    {Topic::FolderMapping, "folderMapping", Section::Files, L10N_KEY("start.folderMapping.title"), L10N_KEY("start.folderMapping.description"), L10N_KEY("start.folderMapping.instructions"), SettingsPage::General, "", true, false},
    {Topic::FileGroup, "fileGroup", Section::Files, L10N_KEY("start.fileGroup.title"), L10N_KEY("start.fileGroup.description"), L10N_KEY("start.fileGroup.instructions"), SettingsPage::General, "", true, false},
    {Topic::CategoryRules, "categoryRules", Section::Files, L10N_KEY("start.categoryRules.title"), L10N_KEY("start.categoryRules.description"), L10N_KEY("start.categoryRules.instructions"), SettingsPage::DesktopCategories, "desktop.categoryRules", false, false},
    {Topic::DockEnable, "dockEnable", Section::Dock, L10N_KEY("start.dockEnable.title"), L10N_KEY("start.dockEnable.description"), L10N_KEY("start.dockEnable.instructions"), SettingsPage::Dock, "dock.enable", false, false},
    {Topic::DockPin, "dockPin", Section::Dock, L10N_KEY("start.dockPin.title"), L10N_KEY("start.dockPin.description"), L10N_KEY("start.dockPin.instructions"), SettingsPage::Dock, "dock.enable", true, true},
    {Topic::DockCollection, "dockCollection", Section::Dock, L10N_KEY("start.dockCollection.title"), L10N_KEY("start.dockCollection.description"), L10N_KEY("start.dockCollection.instructions"), SettingsPage::Dock, "dock.enable", true, true},
    {Topic::DockFiles, "dockFiles", Section::Dock, L10N_KEY("start.dockFiles.title"), L10N_KEY("start.dockFiles.description"), L10N_KEY("start.dockFiles.instructions"), SettingsPage::Dock, "dock.enable", true, true},
    {Topic::DockPosition, "dockPosition", Section::Dock, L10N_KEY("start.dockPosition.title"), L10N_KEY("start.dockPosition.description"), L10N_KEY("start.dockPosition.instructions"), SettingsPage::Dock, "dock.position", false, false},
    {Topic::DockSummon, "dockSummon", Section::Dock, L10N_KEY("start.dockSummon.title"), L10N_KEY("start.dockSummon.description"), L10N_KEY("start.dockSummon.instructions"), SettingsPage::Dock, "dock.showOnlyWhenSummoned", false, false},
    {Topic::DockHotkeys, "dockHotkeys", Section::Dock, L10N_KEY("start.dockHotkeys.title"), L10N_KEY("start.dockHotkeys.description"), L10N_KEY("start.dockHotkeys.instructions"), SettingsPage::Dock, "dock.floatingShortcutMode", false, false},
    {Topic::DockSpace, "dockSpace", Section::Dock, L10N_KEY("start.dockSpace.title"), L10N_KEY("start.dockSpace.description"), L10N_KEY("start.dockSpace.instructions"), SettingsPage::Dock, "dock.allowDesktopContentOverlap", false, false},
    {Topic::DockAppearance, "dockAppearance", Section::Dock, L10N_KEY("start.dockAppearance.title"), L10N_KEY("start.dockAppearance.description"), L10N_KEY("start.dockAppearance.instructions"), SettingsPage::AppearanceTheme, "personalization.dockAppearance", false, false, SettingsPage::AnimationPerformance, "animation.hover", L10N_KEY("settings.nav.animation")},
    {Topic::Navigation, "navigation", Section::Dock, L10N_KEY("start.navigation.title"), L10N_KEY("start.navigation.description"), L10N_KEY("start.navigation.instructions"), SettingsPage::General, "general.quickNavigation", false, false},
    {Topic::LuaWidget, "luaWidget", Section::More, L10N_KEY("start.luaWidget.title"), L10N_KEY("start.luaWidget.description"), L10N_KEY("start.luaWidget.instructions"), SettingsPage::Widgets, "widgets.included", true, false},
    {Topic::Workshop, "workshop", Section::More, L10N_KEY("start.workshop.title"), L10N_KEY("start.workshop.description"), L10N_KEY("start.workshop.instructions"), SettingsPage::Widgets, "widgets.workshop", false, false},
    {Topic::Develop, "develop", Section::More, L10N_KEY("start.develop.title"), L10N_KEY("start.develop.description"), L10N_KEY("start.develop.instructions"), SettingsPage::DeveloperTools, "developer.agentSkill", false, false},
    {Topic::PagesOverview, "pagesOverview", Section::Pages, L10N_KEY("start.pagesOverview.title"), L10N_KEY("start.pagesOverview.description"), L10N_KEY("start.pagesOverview.instructions"), SettingsPage::DesktopPages, "pages.order", false},
    {Topic::PagesManage, "pagesManage", Section::Pages, L10N_KEY("start.pagesManage.title"), L10N_KEY("start.pagesManage.description"), L10N_KEY("start.pagesManage.instructions"), SettingsPage::DesktopPages, "pages.order", true},
    {Topic::PagesEdge, "pagesEdge", Section::Pages, L10N_KEY("start.pagesEdge.title"), L10N_KEY("start.pagesEdge.description"), L10N_KEY("start.pagesEdge.instructions"), SettingsPage::DesktopPages, "pages.add", true},
    {Topic::PagesDrag, "pagesDrag", Section::Pages, L10N_KEY("start.pagesDrag.title"), L10N_KEY("start.pagesDrag.description"), L10N_KEY("start.pagesDrag.instructions"), SettingsPage::DesktopPages, "pages.add", true},
    {Topic::PagesKeys, "pagesKeys", Section::Pages, L10N_KEY("start.pagesKeys.title"), L10N_KEY("start.pagesKeys.description"), L10N_KEY("start.pagesKeys.instructions"), SettingsPage::DesktopPages, "general.pageNavigation", false},
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
    case Section::Basics: return L10N_KEY("start.basics");
    case Section::Files: return L10N_KEY("start.section.files");
    case Section::Dock: return L10N_KEY("start.section.dock");
    case Section::More: return L10N_KEY("start.section.lua");
    case Section::Pages: return L10N_KEY("start.section.pages");
    }
    return L10N_KEY("start.title");
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
