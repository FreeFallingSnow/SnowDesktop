#pragma once

#include "l10n.h"
#include "settings_route.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// Private settings help. Practice navigation is process-local; no completion
// history or widget identity is persisted.
namespace snowdesktop::usage_guide
{
enum class Section { Settings, Basics, Files, Dock, Navigation, More };
enum class Topic { Startup, Grid, Icons, Beautify, Theme, Collection, Application, Move, Resize, CollectionGroup, Files, FolderMapping, FileGroup, DockPin, DockMapping, DockCollection, DockFiles, DockSummon, Navigation, LuaWidget, Backup };
// Read-only availability, recomputed from the current desktop, never persisted.
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
inline constexpr std::array<Lesson, 21> kLessons{{
    {Topic::Startup, "startup", Section::Basics, L10N_KEY("start.startup.title"), L10N_KEY("start.startup.description"), L10N_KEY("start.startup.instructions"), L10N_KEY("start.startup.tips"), L10N_KEY("start.startup.hint"), 0, SettingsPage::General, "general.autoStart", false},
    {Topic::Grid, "grid", Section::Settings, L10N_KEY("start.grid.title"), L10N_KEY("start.grid.description"), L10N_KEY("start.grid.instructions"), L10N_KEY("start.grid.tips"), L10N_KEY("start.grid.hint"), 0, SettingsPage::DesktopPages, "pages.grid", false},
    {Topic::Icons, "icons", Section::Settings, L10N_KEY("start.icons.title"), L10N_KEY("start.icons.description"), L10N_KEY("start.icons.instructions"), L10N_KEY("start.icons.tips"), L10N_KEY("start.icons.hint"), 0, SettingsPage::AppearanceDesktopIcons, "", false},
    {Topic::Beautify, "beautify", Section::Settings, L10N_KEY("start.beautify.title"), L10N_KEY("start.beautify.description"), L10N_KEY("start.beautify.instructions"), L10N_KEY("start.beautify.tips"), L10N_KEY("start.beautify.hint"), 0, SettingsPage::AppearanceIconBeautification, "", false},
    {Topic::Theme, "theme", Section::Settings, L10N_KEY("start.theme.title"), L10N_KEY("start.theme.description"), L10N_KEY("start.theme.instructions"), L10N_KEY("start.theme.tips"), L10N_KEY("start.theme.hint"), 0, SettingsPage::AppearanceTheme, "", false},
    {Topic::Collection, "collection", Section::Basics, L10N_KEY("start.collection.title"), L10N_KEY("start.collection.description"), L10N_KEY("start.collection.instructions"), L10N_KEY("start.collection.tips"), L10N_KEY("start.collection.hint"), 0, SettingsPage::General, "", true},
    {Topic::Application, "application", Section::Basics, L10N_KEY("start.application.title"), L10N_KEY("start.application.description"), L10N_KEY("start.application.instructions"), L10N_KEY("start.application.tips"), L10N_KEY("start.application.hint"), 1, SettingsPage::General, "", true},
    {Topic::Move, "move", Section::Basics, L10N_KEY("start.move.title"), L10N_KEY("start.move.description"), L10N_KEY("start.move.instructions"), L10N_KEY("start.move.tips"), L10N_KEY("start.move.hint"), 4, SettingsPage::General, "", true},
    {Topic::Resize, "resize", Section::Basics, L10N_KEY("start.resize.title"), L10N_KEY("start.resize.description"), L10N_KEY("start.resize.instructions"), L10N_KEY("start.resize.tips"), L10N_KEY("start.resize.hint"), 4, SettingsPage::General, "", true},
    {Topic::CollectionGroup, "collectionGroup", Section::Basics, L10N_KEY("start.collectionGroup.title"), L10N_KEY("start.collectionGroup.description"), L10N_KEY("start.collectionGroup.instructions"), L10N_KEY("start.collectionGroup.tips"), L10N_KEY("start.collectionGroup.hint"), 2, SettingsPage::General, "", true},
    {Topic::Files, "files", Section::Files, L10N_KEY("start.files.title"), L10N_KEY("start.files.description"), L10N_KEY("start.files.instructions"), L10N_KEY("start.files.tips"), L10N_KEY("start.files.hint"), 0, SettingsPage::DesktopCategories, "", true},
    {Topic::FolderMapping, "folderMapping", Section::Files, L10N_KEY("start.folderMapping.title"), L10N_KEY("start.folderMapping.description"), L10N_KEY("start.folderMapping.instructions"), L10N_KEY("start.folderMapping.tips"), L10N_KEY("start.folderMapping.hint"), 0, SettingsPage::General, "", true},
    {Topic::FileGroup, "fileGroup", Section::Files, L10N_KEY("start.fileGroup.title"), L10N_KEY("start.fileGroup.description"), L10N_KEY("start.fileGroup.instructions"), L10N_KEY("start.fileGroup.tips"), L10N_KEY("start.fileGroup.hint"), 16, SettingsPage::General, "", true},
    {Topic::DockPin, "dockPin", Section::Dock, L10N_KEY("start.dockPin.title"), L10N_KEY("start.dockPin.description"), L10N_KEY("start.dockPin.instructions"), L10N_KEY("start.dockPin.tips"), L10N_KEY("start.dockPin.hint"), 8, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockMapping, "dockMapping", Section::Dock, L10N_KEY("start.dockMapping.title"), L10N_KEY("start.dockMapping.description"), L10N_KEY("start.dockMapping.instructions"), L10N_KEY("start.dockMapping.tips"), L10N_KEY("start.dockMapping.hint"), 8, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockCollection, "dockCollection", Section::Dock, L10N_KEY("start.dockCollection.title"), L10N_KEY("start.dockCollection.description"), L10N_KEY("start.dockCollection.instructions"), L10N_KEY("start.dockCollection.tips"), L10N_KEY("start.dockCollection.hint"), 40, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockFiles, "dockFiles", Section::Dock, L10N_KEY("start.dockFiles.title"), L10N_KEY("start.dockFiles.description"), L10N_KEY("start.dockFiles.instructions"), L10N_KEY("start.dockFiles.tips"), L10N_KEY("start.dockFiles.hint"), 24, SettingsPage::Dock, "dock.enable", true},
    {Topic::DockSummon, "dockSummon", Section::Dock, L10N_KEY("start.dockSummon.title"), L10N_KEY("start.dockSummon.description"), L10N_KEY("start.dockSummon.instructions"), L10N_KEY("start.dockSummon.tips"), L10N_KEY("start.dockSummon.hint"), 8, SettingsPage::Dock, "dock.showOnlyWhenSummoned", true},
    {Topic::Navigation, "navigation", Section::Navigation, L10N_KEY("start.navigation.title"), L10N_KEY("start.navigation.description"), L10N_KEY("start.navigation.instructions"), L10N_KEY("start.navigation.tips"), L10N_KEY("start.navigation.hint"), 64, SettingsPage::General, "general.quickNavigation", true},
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
    if (key == "layout") return Topic::Move; // Old private route.
    for (const auto& lesson : kLessons) if (key == lesson.key) return lesson.topic;
    return std::nullopt;
}
inline std::optional<Context> MissingPrerequisite(const Lesson& lesson, std::uint32_t context)
{
    for (auto condition : {DockEnabled, TwoStandaloneCollections, StandaloneCollection,
            CollectionAvailable, StandaloneFileSource, StandaloneWidget, NavigationEnabled})
        if ((lesson.required & condition) && !(context & condition)) return condition;
    return std::nullopt;
}
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
inline SettingsRoute PrerequisiteRoute(Context context)
{
    if (context == DockEnabled) return SettingsRoute::ForPage(SettingsPage::Dock, "dock.enable");
    if (context == NavigationEnabled) return SettingsRoute::ForPage(SettingsPage::General, "general.quickNavigation");
    if (context == StandaloneFileSource) return SettingsRoute::ForPage(SettingsPage::General, "start.files");
    return SettingsRoute::ForPage(SettingsPage::General, "start.collection");
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
    case Section::More: return L10N_KEY("start.section.more");
    }
    return L10N_KEY("start.title");
}
inline bool HasSettings(const Lesson& lesson)
{
    return !lesson.practice || lesson.settingsPage != SettingsPage::General || *lesson.settingsFocus;
}
inline std::optional<Topic> Adjacent(Topic topic, bool forward)
{
    const auto* selected = Find(topic);
    if (!selected) return {};
    std::optional<Topic> previous;
    bool found = false;
    for (const auto& lesson : kLessons)
    {
        if (lesson.section != selected->section) continue;
        if (found) return lesson.topic;
        if (lesson.topic == topic)
        {
            if (!forward) return previous;
            found = true;
        }
        previous = lesson.topic;
    }
    return {};
}
inline std::optional<Topic> NextSection(Topic topic)
{
    const auto* selected = Find(topic);
    if (!selected) return {};
    for (std::size_t i = 0; i + 1 < kSections.size(); ++i)
        if (kSections[i] == selected->section)
            for (const auto& lesson : kLessons)
                if (lesson.section == kSections[i + 1]) return lesson.topic;
    return {};
}
inline std::optional<Topic> PreparationLesson(Context context)
{
    if (context == DockEnabled || context == NavigationEnabled) return {};
    return context == StandaloneFileSource ? Topic::Files : Topic::Collection;
}
enum class AdvanceResult { Unavailable, Prerequisite, Prepared, Advanced, SectionEnd };
struct Practice
{
    std::optional<Topic> active;
    bool paused = false;
    bool sectionEnd = false;
    std::optional<Context> preparation;
    bool Visible() const { return active.has_value() && !paused; }
    bool Begin(Topic topic, std::uint32_t context = 0)
    {
        const auto* lesson = Find(topic);
        if (!lesson) return false;
        // Missing prerequisites are taught in place. Starting never completes
        // a lesson, creates a widget or changes a setting.
        active = topic;
        paused = sectionEnd = false;
        preparation = MissingPrerequisite(*lesson, context);
        return true;
    }
    void ObserveContext(std::uint32_t context)
    {
        // Preparation is an entry condition. A successful operation may itself
        // consume the source (grouping it or moving it into Dock).
        if (active && preparation && !sectionEnd)
            if (const auto missing = MissingPrerequisite(*Find(*active), context)) preparation = missing;
    }
    bool Resume(std::uint32_t context)
    {
        if (!active) return false;
        paused = false; ObserveContext(context); return true;
    }
    void Pause() { if (active) paused = true; }
    AdvanceResult Advance(std::uint32_t context, bool skip = false)
    {
        if (!Visible() || sectionEnd) return AdvanceResult::Unavailable;
        ObserveContext(context);
        if (!skip && preparation && MissingPrerequisite(*Find(*active), context)) return AdvanceResult::Prerequisite;
        if (!skip && preparation) { preparation.reset(); return AdvanceResult::Prepared; }
        preparation.reset();
        if (const auto next = Adjacent(*active, true))
        {
            active = *next;
            preparation = MissingPrerequisite(*Find(*active), context);
            return AdvanceResult::Advanced;
        }
        sectionEnd = true;
        return AdvanceResult::SectionEnd;
    }
    bool Previous(std::uint32_t context)
    {
        if (!Visible()) return false;
        if (sectionEnd) { sectionEnd = false; ObserveContext(context); return true; }
        if (const auto previous = Adjacent(*active, false))
        { active = *previous; preparation = MissingPrerequisite(*Find(*active), context); return true; }
        return false;
    }
    bool ContinueSection(std::uint32_t context)
    {
        if (!Visible() || !sectionEnd) return false;
        if (const auto next = NextSection(*active)) return Begin(*next, context);
        return false;
    }
    std::optional<Topic> End()
    {
        const auto previous = active;
        active.reset();
        paused = sectionEnd = false;
        preparation.reset();
        return previous;
    }
};
// The only persisted preference is the outer panel's expansion state.
bool LoadExpanded(const std::filesystem::path& path, bool& expanded, std::string* error = nullptr);
bool SaveExpanded(const std::filesystem::path& path, bool expanded, std::string* error = nullptr);
}
