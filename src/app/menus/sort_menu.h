#pragma once

#include "common/constants.h"
#include "layout/folder_sort_rules.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "ui/menu/modern_menu.h"

#include <array>
#include <algorithm>
#include <optional>

namespace snowdesktop::sort_menu
{
struct Row
{
    UINT labelCommand;
    UINT ascendingCommand;
    UINT descendingCommand;
    int mode;
    const char* labelKey;
    const wchar_t* glyph;
};

// Label commands also sort ascending, so every selectable cell has an action.
inline constexpr std::array<Row, 4> kRows{{
    {41830, kContextWidgetSortByName, kContextWidgetSortByNameDesc,
        folder_sort_rules::kName, "app.menu.sort_name", menu_fluent_glyphs::kSortName},
    {41831, kContextWidgetSortByType, kContextWidgetSortByTypeDesc,
        folder_sort_rules::kType, "app.menu.sort_type", menu_fluent_glyphs::kSortType},
    {41832, kContextWidgetSortBySize, kContextWidgetSortBySizeDesc,
        folder_sort_rules::kSize, "app.menu.sort_size", menu_fluent_glyphs::kSort},
    {41833, kContextWidgetSortByDate, kContextWidgetSortByDateDesc,
        folder_sort_rules::kModified, "app.interact.sort_date", menu_fluent_glyphs::kSortDate},
}};

struct Selection
{
    int mode;
    bool ascending;
};

inline std::optional<Selection> ResolveCommand(UINT command)
{
    for (const auto& row : kRows)
    {
        if (command == row.labelCommand || command == row.ascendingCommand)
            return Selection{row.mode, true};
        if (command == row.descendingCommand)
            return Selection{row.mode, false};
    }
    return std::nullopt;
}

inline void ApplyInlineLayout(std::vector<modern_menu::Item>& items)
{
    for (const auto& row : kRows)
    {
        auto first = std::find_if(items.begin(), items.end(), [&](const auto& item) {
            return item.command == row.labelCommand && item.children.empty();
        });
        if (first == items.end() || items.end() - first < 3 ||
            first[1].command != row.ascendingCommand ||
            first[2].command != row.descendingCommand)
            continue;
        for (int i = 0; i < 3; ++i)
        {
            first[i].inlineAction = true;
            first[i].inlineGroup = row.labelCommand;
            first[i].compactInlineAction = i != 0;
            first[i].measureInlineAction = true;
            first[i].fitInlineActionToContent = i != 0;
        }
    }
}

template<class LabelLookup>
inline std::vector<modern_menu::Item> BuildItems(const LabelLookup& label)
{
    std::vector<modern_menu::Item> items;
    for (const auto& row : kRows)
    {
        items.push_back({row.labelCommand, label(row.labelKey), row.glyph});
        items.push_back({row.ascendingCommand, label("app.menu.sort_asc")});
        items.push_back({row.descendingCommand, label("app.menu.sort_desc")});
    }
    ApplyInlineLayout(items);
    return items;
}

template<class LabelLookup>
inline HMENU Create(const LabelLookup& label)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;
    for (const auto& item : BuildItems(label))
    {
        if (!AppendMenuW(menu, MF_STRING, item.command, item.label.c_str()))
        {
            DestroyMenu(menu);
            return nullptr;
        }
    }
    return menu;
}

template<class SetIcon>
inline void SetIcons(HMENU menu, const SetIcon& setIcon)
{
    for (const auto& row : kRows)
        setIcon(menu, row.labelCommand, row.glyph);
}
} // namespace snowdesktop::sort_menu
