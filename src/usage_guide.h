#pragma once

#include "l10n.h"
#include "settings_route.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// Private reference content. Only panel expansion is persisted.
namespace snowdesktop::usage_guide
{
enum class Section { Settings, Basics, Files, Dock, Navigation, More };
enum class Topic { Startup, Grid, Icons, Beautify, Theme, Collection, Application, Move, Resize, CollectionGroup, Files, FolderMapping, FileGroup, DockPin, DockMapping, DockCollection, DockFiles, DockSummon, Navigation, LuaWidget, Backup };
// Declarative prerequisites, shown as reference text, never a progress gate.
enum Context : std::uint32_t {
    CollectionAvailable = 1, TwoStandaloneCollections = 2, StandaloneWidget = 4,
    DockEnabled = 8, StandaloneFileSource = 16, StandaloneCollection = 32, NavigationEnabled = 64
};
struct Lesson
{
    Topic topic;
    const char* key;
    Section section;
    const char* title;
    const char* description;
    const char* instructions;
    const char* tips;
    const char* hint;
    std::uint32_t required;
    SettingsPage settingsPage;
    const char* settingsFocus;
    bool practice;
};
inline constexpr std::array<Lesson, 19> kLessons{{
    {Topic::Startup, "startup", Section::Basics, L10N_KEY("start.startup.title"), L10N_KEY("start.startup.description"), L10N_KEY("start.startup.instructions"), L10N_KEY("start.startup.tips"), L10N_KEY("start.startup.hint"), 0, SettingsPage::General, "general.autoStart", false},
    {Topic::Grid, "grid", Section::Settings, L10N_KEY("start.grid.title"), L10N_KEY("start.grid.description"), L10N_KEY("start.grid.instructions"), L10N_KEY("start.grid.tips"), L10N_KEY("start.grid.hint"), 0, SettingsPage::DesktopPages, "pages.grid", false},
    {Topic::Icons, "icons", Section::Settings, L10N_KEY("start.icons.title"), L10N_KEY("start.icons.description"), L10N_KEY("start.icons.instructions"), L10N_KEY("start.icons.tips"), L10N_KEY("start.icons.hint"), 0, SettingsPage::AppearanceDesktopIcons, "", false},
    {Topic::Beautify, "beautify", Section::Settings, L10N_KEY("start.beautify.title"), L10N_KEY("start.beautify.description"), L10N_KEY("start.beautify.instructions"), L10N_KEY("start.beautify.tips"), L10N_KEY("start.beautify.hint"), 0, SettingsPage::AppearanceIconBeautification, "", false},
    {Topic::Theme, "theme", Section::Settings, L10N_KEY("start.theme.title"), L10N_KEY("start.theme.description"), L10N_KEY("start.theme.instructions"), L10N_KEY("start.theme.tips"), L10N_KEY("start.theme.hint"), 0, SettingsPage::AppearanceTheme, "", false},
    {Topic::Collection, "collection", Section::Basics, L10N_KEY("start.collection.title"), L10N_KEY("start.collection.description"), L10N_KEY("start.collection.instructions"), L10N_KEY("start.collection.tips"), L10N_KEY("start.collection.hint"), 0, SettingsPage::General, "", true},
    {Topic::Move, "move", Section::Basics, L10N_KEY("start.move.title"), L10N_KEY("start.move.description"), L10N_KEY("start.move.instructions"), L10N_KEY("start.move.tips"), L10N_KEY("start.move.hint"), 4, SettingsPage::General, "", true},
    {Topic::CollectionGroup, "collectionGroup", Section::Basics, L10N_KEY("start.collectionGroup.title"), L10N_KEY("start.collectionGroup.description"), L10N_KEY("start.collectionGroup.instructions"), L10N_KEY("start.collectionGroup.tips"), L10N_KEY("start.collectionGroup.hint"), 2, SettingsPage::General, "", true},
    {Topic::Files, "files", Section::Files, L10N_KEY("start.files.title"), L10N_KEY("start.files.description"), L10N_KEY("start.files.instructions"), L10N_KEY("start.files.tips"), L10N_KEY("start.files.hint"), 0, SettingsPage::General, "", true},
    {Topic::FolderMapping, "folderMapping", Section::Files, L10N_KEY("start.folderMapping.title"), L10N_KEY("start.folderMapping.description"), L10N_KEY("start.folderMapping.instructions"), L10N_KEY("start.folderMapping.tips"), L10N_KEY("start.folderMapping.hint"), 0, SettingsPage::General, "", true},
    {Topic::FileGroup, "fileGroup", Section::Files, L10N_KEY("start.fileGroup.title"), L10N_KEY("start.fileGroup.description"), L10N_KEY("start.fileGroup.instructions"), L10N_KEY("start.fileGroup.tips"), L10N_KEY("start.fileGroup.hint"), 16, SettingsPage::General, "", true},
    {Topic::DockPin, "dockPin", Section::Dock, L10N_KEY("start.dockPin.title"), L10N_KEY("start.dockPin.description"), L10N_KEY("start.dockPin.instructions"), L10N_KEY("start.dockPin.tips"), L10N_KEY("start.dockPin.hint"), 8, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockMapping, "dockMapping", Section::Dock, L10N_KEY("start.dockMapping.title"), L10N_KEY("start.dockMapping.description"), L10N_KEY("start.dockMapping.instructions"), L10N_KEY("start.dockMapping.tips"), L10N_KEY("start.dockMapping.hint"), 8, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockCollection, "dockCollection", Section::Dock, L10N_KEY("start.dockCollection.title"), L10N_KEY("start.dockCollection.description"), L10N_KEY("start.dockCollection.instructions"), L10N_KEY("start.dockCollection.tips"), L10N_KEY("start.dockCollection.hint"), 40, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockFiles, "dockFiles", Section::Dock, L10N_KEY("start.dockFiles.title"), L10N_KEY("start.dockFiles.description"), L10N_KEY("start.dockFiles.instructions"), L10N_KEY("start.dockFiles.tips"), L10N_KEY("start.dockFiles.hint"), 8, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockSummon, "dockSummon", Section::Dock, L10N_KEY("start.dockSummon.title"), L10N_KEY("start.dockSummon.description"), L10N_KEY("start.dockSummon.instructions"), L10N_KEY("start.dockSummon.tips"), L10N_KEY("start.dockSummon.hint"), 8, SettingsPage::Dock, "dock.showOnlyWhenSummoned", false},
    {Topic::Navigation, "navigation", Section::Navigation, L10N_KEY("start.navigation.title"), L10N_KEY("start.navigation.description"), L10N_KEY("start.navigation.instructions"), L10N_KEY("start.navigation.tips"), L10N_KEY("start.navigation.hint"), 64, SettingsPage::General, "general.quickNavigation", false},
    {Topic::LuaWidget, "luaWidget", Section::More, L10N_KEY("start.luaWidget.title"), L10N_KEY("start.luaWidget.description"), L10N_KEY("start.luaWidget.instructions"), L10N_KEY("start.luaWidget.tips"), L10N_KEY("start.luaWidget.hint"), 0, SettingsPage::Widgets, "", true},
    {Topic::Backup, "backup", Section::More, L10N_KEY("start.backup.title"), L10N_KEY("start.backup.description"), L10N_KEY("start.backup.instructions"), L10N_KEY("start.backup.tips"), L10N_KEY("start.backup.hint"), 0, SettingsPage::BackupAndData, "", false},
}};
inline const Lesson* Find(Topic topic)
{
    for (const auto& lesson : kLessons) if (lesson.topic == topic) return &lesson;
    return nullptr;
}
inline std::optional<Topic> ParseTopic(std::string_view key)
{
    if (key == "application") return Topic::Collection;
    if (key == "layout" || key == "resize") return Topic::Move; // Old private route.
    for (const auto& lesson : kLessons) if (key == lesson.key) return lesson.topic;
    return std::nullopt;
}
inline constexpr std::array<Context, 7> kPrerequisites{
    DockEnabled, TwoStandaloneCollections, StandaloneCollection,
    CollectionAvailable, StandaloneFileSource, StandaloneWidget, NavigationEnabled};
inline const char* PrerequisiteText(Context context)
{
    switch (context)
    {
    case DockEnabled: return L10N_KEY("start.requires.dock");
    case TwoStandaloneCollections: return L10N_KEY("start.requires.twoCollections");
    case StandaloneCollection: return L10N_KEY("start.requires.standaloneCollection");
    case CollectionAvailable: return L10N_KEY("start.requires.collection");
    case StandaloneFileSource: return L10N_KEY("start.requires.fileSource");
    case StandaloneWidget: return L10N_KEY("start.requires.widget");
    case NavigationEnabled: return L10N_KEY("start.requires.navigation");
    }
    return L10N_KEY("start.error.unavailable");
}
inline constexpr std::array<Section, 6> kSections{
    Section::Basics, Section::Files, Section::Settings, Section::Dock, Section::Navigation, Section::More};
inline const char* SectionTitle(Section section)
{
    switch (section)
    {
    case Section::Basics: return L10N_KEY("start.basics");
    case Section::Files: return L10N_KEY("start.section.files");
    case Section::Settings: return L10N_KEY("start.section.settings");
    case Section::Dock: return L10N_KEY("start.section.dock");
    case Section::Navigation: return L10N_KEY("start.section.navigation");
    case Section::More: return L10N_KEY("app.settings.widgets");
    }
    return L10N_KEY("start.title");
}
inline bool HasSettings(const Lesson& lesson)
{
    return !lesson.practice || lesson.settingsPage != SettingsPage::General || *lesson.settingsFocus;
}
// The only persisted preference is the outer panel's expansion state.
bool LoadExpanded(const std::filesystem::path& path, bool& expanded, std::string* error = nullptr);
bool SaveExpanded(const std::filesystem::path& path, bool expanded, std::string* error = nullptr);
}
