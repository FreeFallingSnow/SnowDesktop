#pragma once
#include "theme_library.h"
#include <array>

namespace snowdesktop::themes::tags
{
// Steamworks category entries must match these bytes exactly. Localized labels
// are presentation only; never send translated labels to SetItemTags.
inline constexpr std::string_view Content = "Theme";
struct Category { std::string_view value, label; };
inline constexpr std::array<Category, 6> Categories{{
    {"Global Theme", "themeLibrary.global"}, {"Dock Theme", "themeLibrary.dock"},
    {"Status Bar Theme", "themeLibrary.statusBar"}, {"Taskbar Theme", "themeLibrary.taskbar"},
    {"Quick Panel Theme", "themeLibrary.quickPanel"}, {"Popup Theme", "themeLibrary.popup"}
}};
inline std::vector<std::string> Applicable(const Theme& theme)
{
    std::vector<std::string> result;
    const auto add = [&](std::size_t index) { result.emplace_back(Categories[index].value); };
    if (theme.kind == Kind::QuickPanel) add(4);
    else if (theme.kind == Kind::Popup) add(5);
    else
    {
        if (FullScope(theme.scopes)) add(0);
        if (theme.scopes & Dock) add(1);
        if (theme.scopes & StatusBar) add(2);
        if (theme.scopes & Taskbar) add(3);
    }
    return result;
}
inline bool Valid(const Theme& theme, const std::vector<std::string>& selected)
{
    auto expected = Applicable(theme), actual = selected;
    if (actual.empty()) return false;
    std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
    return expected == actual; // No missing scope, duplicate or foreign label.
}
}
