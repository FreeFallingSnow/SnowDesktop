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

struct EditSource
{
    std::optional<themes::Theme> theme;
    void Reset() { theme.reset(); }
    void Begin(const themes::Library& library, std::string_view target)
    {
        Reset();
        const auto found = library.references.find(std::string(target));
        if (found != library.references.end() && !found->second.id.empty())
            theme = themes::Resolve(found->second.snapshot, found->second.id);
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
