#pragma once
#include "theme_library.h"

namespace snowdesktop::themes::preview
{
struct Part { std::string component, themeId; };
// A full global theme also demonstrates its surface on the real mapped-folder
// component. This sample does not expand the theme's application scope.
inline std::vector<Part> Parts(const Package& package, std::string_view root, unsigned scope, std::string& error)
{
    if (!Validate(package, error)) return {};
    const auto theme = Resolve(package, root);
    if (!theme) { error = "themeNotFound"; return {}; }
    if (theme->kind == Kind::QuickPanel) return {{"quick-navigation", std::string(root)}};
    if (theme->kind == Kind::Popup) return {{"popup", std::string(root)}, {"control-panel", std::string(root)}};
    if (!scope || (scope & ~All) || (theme->scopes & scope) != scope) { error = "invalidSelection"; return {}; }
    std::vector<Part> out;
    if ((scope & Components) || (scope == theme->scopes && FullScope(scope))) out.push_back({"folder-mapping", std::string(root)});
    for (const auto& [bit, component] : std::vector<std::pair<unsigned, std::string>>{
        {Dock, "dock"}, {StatusBar, "status-bar"}, {Taskbar, "taskbar"}})
        if (scope & bit) out.push_back({component, std::string(root)});
    if (scope == theme->scopes && FullScope(theme->scopes))
    {
        out.push_back({"quick-navigation", theme->quickPanel}); out.push_back({"popup", theme->popup});
        out.push_back({"control-panel", theme->popup});
    }
    return out;
}
inline std::string GalleryPrefix(std::string_view rootHash) { return "SnowDesktop-theme-" + std::string(rootHash) + "-"; }
inline std::string GalleryFilename(std::string_view rootHash, std::string_view component)
{ return GalleryPrefix(rootHash) + std::string(component) + ".png"; }
inline bool ManagedGalleryFilename(std::string_view name, std::string_view prefix)
{
    if (prefix.empty() || !name.starts_with(prefix)) return false;
    name.remove_prefix(prefix.size());
    for (const auto component : {"folder-mapping.png", "dock.png", "status-bar.png", "taskbar.png", "quick-navigation.png", "popup.png", "control-panel.png"})
        if (name == component) return true;
    return false;
}
inline bool ReplaceableGalleryFilename(std::string_view name, std::string_view currentPrefix, std::string_view previousPrefix)
{
    return ManagedGalleryFilename(name, currentPrefix) ||
        (!previousPrefix.empty() && ManagedGalleryFilename(name, previousPrefix));
}
}
