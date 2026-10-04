#pragma once
#include "../theme_library.h"

namespace snowdesktop::winui::theme_controls
{
// Private settings state: no package format or component API changes.
enum class FilterTab { All, Global, Dock, StatusBar, Taskbar, QuickPanel, Popup };
inline std::filesystem::path TaskDirectory(const std::filesystem::path& temporary, std::string_view id)
{
    // IDs contain "theme/". Use only the leaf, so Render's single-directory
    // isolation guard works without creating an unintended parent directory.
    if (id.empty()) return {};
    const auto leaf = std::filesystem::path(std::string(id)).filename();
    return temporary / (L"SnowDesktop-theme-" + leaf.wstring());
}
inline bool MatchesFilter(const themes::Theme& theme, FilterTab tab)
{
    using namespace themes;
    switch (tab)
    {
    case FilterTab::All: return true;
    case FilterTab::Global: return theme.kind == Kind::Global && FullScope(theme.scopes);
    case FilterTab::Dock: return theme.kind == Kind::Global && (theme.scopes & themes::Dock);
    case FilterTab::StatusBar: return theme.kind == Kind::Global && (theme.scopes & themes::StatusBar);
    case FilterTab::Taskbar: return theme.kind == Kind::Global && (theme.scopes & themes::Taskbar);
    case FilterTab::QuickPanel: return theme.kind == Kind::QuickPanel;
    case FilterTab::Popup: return theme.kind == Kind::Popup;
    }
    return false;
}
inline std::string VersionCounterpart(const themes::Library& library,
    const std::map<std::string, std::string>& urls, const themes::Theme& theme)
{
    const auto subscribed = [&](const std::string& id) {
        for (const auto& [item, origin] : library.workshop) { (void)item; if (origin.ids.contains(id)) return true; }
        return false;
    };
    std::string item;
    for (const auto& [key, origin] : library.workshop) if (origin.ids.contains(theme.id)) item = key;
    const bool installed = !item.empty();
    if (item.empty())
    {
        const auto url = urls.find(theme.id); if (url == urls.end()) return {};
        constexpr std::string_view prefix = "https://steamcommunity.com/sharedfiles/filedetails/?id=";
        if (!url->second.starts_with(prefix)) return {};
        item = url->second.substr(prefix.size());
    }
    const auto origin = library.workshop.find(item); if (origin == library.workshop.end()) return {};
    std::string match;
    for (const auto& [id, candidate] : library.themes)
    {
        if (id == theme.id || candidate.kind != theme.kind || candidate.scopes != theme.scopes) continue;
        bool pair = !installed && origin->second.ids.contains(id);
        if (installed && !subscribed(id))
        {
            const auto url = urls.find(id);
            pair = url != urls.end() && url->second == "https://steamcommunity.com/sharedfiles/filedetails/?id=" + item;
        }
        if (pair) { if (!match.empty()) return {}; match = id; }
    }
    return match;
}

struct EditSource
{
    std::optional<themes::Theme> theme;
    themes::Package snapshot;
    void Reset() { theme.reset(); snapshot.clear(); }
    void Begin(const themes::Library& library, std::string_view target)
    {
        Reset();
        const auto found = library.references.find(std::string(target));
        if (found != library.references.end() && !found->second.id.empty())
        {
            snapshot = found->second.snapshot;
            theme = themes::Resolve(snapshot, found->second.id);
        }
    }
    EditSource Bound(themes::Kind kind) const
    {
        EditSource child;
        if (!theme || theme->kind != themes::Kind::Global || !themes::FullScope(theme->scopes)) return child;
        const auto& id = kind == themes::Kind::QuickPanel ? theme->quickPanel : theme->popup;
        child.theme = themes::Resolve(snapshot, id);
        if (child.theme && child.theme->kind == kind) child.snapshot = snapshot;
        else child.Reset();
        return child;
    }
    bool CanUpdate(const themes::Library& library) const
    {
        if (!theme || themes::Builtin(theme->id)) return false;
        const auto saved = themes::Find(library.themes, theme->id);
        if (!saved || saved->kind != theme->kind) return false;
        for (const auto& [item, origin] : library.workshop)
        {
            (void)item;
            if (origin.ids.contains(theme->id)) return false;
        }
        return true;
    }
};
}
