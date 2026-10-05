#pragma once
#include "../theme_library.h"
#include <algorithm>

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
    std::set<std::string> matches;
    const auto add = [&](const std::string& id) {
        const auto candidate = themes::Find(library.themes, id);
        if (id != theme.id && candidate && candidate->kind == theme.kind) matches.insert(id);
    };
    for (const auto& [item, origin] : library.workshop)
    {
        const auto url = "https://steamcommunity.com/sharedfiles/filedetails/?id=" + item;
        if (origin.ids.contains(theme.id))
        {
            if (const auto source = origin.sourceIds.find(theme.id); source != origin.sourceIds.end() && !subscribed(source->second)) add(source->second);
            for (const auto& [id, address] : urls) if (!subscribed(id) && address == url) add(id);
        }
        else if (!subscribed(theme.id))
        {
            for (const auto& [id, authored] : origin.sourceIds) if (authored == theme.id) add(id);
            if (const auto address = urls.find(theme.id); address != urls.end() && address->second == url)
                for (const auto& id : origin.ids) add(id);
        }
    }
    return matches.size() == 1 ? *matches.begin() : std::string{};
}
inline std::vector<themes::Theme> ManagementEntries(const themes::Library& library,
    const std::map<std::string, std::string>& urls, const std::map<std::string, std::string>& versions, FilterTab filter)
{
    const auto subscribed = [&](const std::string& id) {
        for (const auto& [item, origin] : library.workshop) { (void)item; if (origin.ids.contains(id)) return true; }
        return false;
    };
    const auto active = [&](const std::string& id) {
        for (const auto& [target, reference] : library.references)
        {
            (void)target;
            if (reference.id == id) return true;
            if (const auto root = themes::Find(reference.snapshot, reference.id); root && (root->quickPanel == id || root->popup == id)) return true;
        }
        return false;
    };
    std::set<std::string> seen;
    std::vector<themes::Theme> entries;
    for (const auto& [id, theme] : library.themes)
    {
        if (themes::Builtin(id) || seen.contains(id)) continue;
        const auto otherId = VersionCounterpart(library, urls, theme);
        const auto other = themes::Find(library.themes, otherId);
        const bool paired = other && VersionCounterpart(library, urls, *other) == id;
        seen.insert(id); if (paired) seen.insert(otherId);
        if (!MatchesFilter(theme, filter) && (!paired || !MatchesFilter(*other, filter))) continue;
        const themes::Theme* chosen = &theme;
        if (paired)
        {
            const auto local = subscribed(id) ? other : &theme;
            const auto remote = subscribed(id) ? &theme : other;
            chosen = local;
            if (const auto preferred = versions.find(local->id); preferred != versions.end())
            { if (preferred->second == remote->id) chosen = remote; }
            else if (active(remote->id) && !active(local->id)) chosen = remote;
        }
        entries.push_back(*chosen);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return entries;
}
inline std::vector<themes::Theme> CollapseVersionChoices(const themes::Library& library,
    const std::map<std::string, std::string>& urls, const std::vector<themes::Theme>& choices,
    std::string_view preferred)
{
    const auto subscribed = [&](const std::string& id) {
        for (const auto& [item, origin] : library.workshop) { (void)item; if (origin.ids.contains(id)) return true; }
        return false;
    };
    std::vector<themes::Theme> result; std::set<std::string> seen;
    for (const auto& theme : choices)
    {
        if (seen.contains(theme.id)) continue;
        const auto otherId = VersionCounterpart(library, urls, theme);
        const auto other = std::find_if(choices.begin(), choices.end(), [&](const auto& entry) { return entry.id == otherId; });
        const themes::Theme* selected = &theme; seen.insert(theme.id);
        if (other != choices.end() && VersionCounterpart(library, urls, *other) == theme.id)
        {
            seen.insert(otherId);
            if (preferred == otherId || (preferred != theme.id && subscribed(theme.id))) selected = &*other;
        }
        result.push_back(*selected);
    }
    return result;
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
inline EditSource SelectionEditSource(const themes::Library& library, const themes::Theme* selected,
    const EditSource& inherited, bool followsGlobal)
{
    EditSource source;
    if (selected)
    {
        source.theme = *selected;
        std::string error;
        if (!themes::Export(library, selected->id, source.snapshot, error)) source.Reset();
    }
    else if (followsGlobal) source = inherited;
    if (!source.CanUpdate(library)) source.Reset();
    return source;
}
}
