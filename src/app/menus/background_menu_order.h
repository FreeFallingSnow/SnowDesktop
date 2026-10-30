#pragma once

#include "common/constants.h"
#include "sort_menu.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "ui/menu/modern_menu.h"
#include "ui/menu/modern_menu_appearance_rules.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

namespace snowdesktop::background_menu_order
{
// Only explicitly marked host background menus participate. Item menus,
// author-defined element menus and every nested cascade keep their own order.
inline constexpr ULONG_PTR kRootTag = 0x53444247;

inline bool Mark(HMENU menu)
{
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_MENUDATA;
    info.dwMenuData = kRootTag;
    return SetMenuInfo(menu, &info) != FALSE;
}

enum class Group { Create, Actions, Custom, Content, Visibility, Behavior, Appearance, Pages, Manage, Shell };
struct Position
{
    Group group = Group::Custom;
    int order = 0;
};

inline bool HasChild(const modern_menu::Item& item, UINT command)
{
    return std::ranges::any_of(item.children, [command](const auto& child) {
        return child.command == command;
    });
}

inline bool ContainsAuthorAction(const modern_menu::Item& item)
{
    if (item.children.empty())
        return item.command >= kContextLuaWidgetMenuFirst && item.command <= kContextLuaWidgetMenuLast;
    return std::ranges::any_of(item.children, [](const auto& child) { return ContainsAuthorAction(child); });
}

inline Position Resolve(const modern_menu::Item& item)
{
    // Author actions keep their order even when they use a host-like glyph.
    if (ContainsAuthorAction(item)) return {};
    // Ordinary parent IDs can originate from opaque HMENU values. Only the
    // explicitly identified New cascade owns a host command in this branch.
    const bool hostCommand = item.children.empty() ||
        (item.command == kContextNewMenu && item.builtinIcon == menu_icon::BuiltinIcon::NewItem);
    switch (hostCommand ? item.command : 0)
    {
    case kContextNewMenu: return {Group::Create, 0};
    case kContextPasteCommand: return {Group::Actions, 0};
    case kContextWidgetOpenFolder: return {Group::Actions, 10};
    case kContextWidgetOpen: return {Group::Actions, 20};
    case kContextWidgetManualCollect: return {Group::Actions, 30};
    case kContextRefreshCommand: return {Group::Actions, 50};
    case kContextWidgetToggleSingleCellMode: return {Group::Content, 0};
    case kContextWidgetToggleCollectionMode: return {Group::Content, 10};
    case kContextPopupFan: return {Group::Content, 20};
    case kContextWidgetToggleListMode: return {Group::Content, 30};
    case kContextWidgetToggleLargeFolderTitleless: return {Group::Content, 70};
    case kContextWidgetToggleFileCategories: return {Group::Visibility, 0};
    case kContextWidgetToggleSearchBox: return {Group::Visibility, 10};
    case kContextWidgetToggleDateGroup: return {Group::Visibility, 20};
    case kContextWidgetShowOnHoverOn:
    case kContextWidgetShowOnHoverOff: return {Group::Behavior, 0};
    case kContextWidgetToggleExpandOnHover: return {Group::Behavior, 10};
    case kContextWidgetKeepWhenHiddenOn:
    case kContextWidgetKeepWhenHiddenOff: return {Group::Behavior, 20};
    case kContextWidgetPrivacyModeOn:
    case kContextWidgetPrivacyModeOff: return {Group::Behavior, 30};
    case kContextPagePrev: return {Group::Pages, 10};
    case kContextPageNext: return {Group::Pages, 20};
    case kContextWidgetEdit:
    case kContextWidgetOpenComponentPanel: return {Group::Manage, 0};
    case kContextWidgetRename: return {Group::Manage, 10};
    case kContextWidgetDelete: return {Group::Manage, 20};
    case kContextSettingsCommand: return {Group::Manage, 30};
    case kContextMoreCommand: return {Group::Shell, 0};
    case kContextManageMenuCommand: return {Group::Shell, 10};
    default: break;
    }
    if (HasChild(item, kContextAddCollectionWidget) ||
        item.builtinIcon == menu_icon::BuiltinIcon::Widgets) return {Group::Create, 10};
    if (HasChild(item, kContextWidgetAutoCollectAll)) return {Group::Actions, 40};
    if (HasChild(item, sort_menu::kRows.front().labelCommand) ||
        HasChild(item, kContextWidgetSortByName)) return {Group::Content, 40};
    if (HasChild(item, kContextWidgetListColumnsFirst)) return {Group::Content, 50};
    if (HasChild(item, kContextWidgetDetailName)) return {Group::Content, 60};
    if (item.builtinIcon == menu_icon::BuiltinIcon::Display) return {Group::Content, 30};
    if (item.glyph == menu_fluent_glyphs::kIconPersonalization) return {Group::Appearance, 0};
    if (HasChild(item, kContextWidgetDemoCategoryFirst)) return {Group::Appearance, 10};
    if (HasChild(item, kContextPinFirstPage)) return {Group::Pages, 0};
    if (std::ranges::any_of(item.children, [](const auto& child) {
            return child.command >= kContextPageJumpFirst && child.command <= kContextPageJumpLast;
        })) return {Group::Pages, 30};
    if (HasChild(item, kContextPageAdd)) return {Group::Pages, 40};
    return {};
}

inline int QuickOrder(UINT command)
{
    switch (command)
    {
    case kContextPasteCommand: return 0;
    case kContextRefreshCommand: return 10;
    case kContextWidgetEdit: return 20;
    case kContextWidgetRename: return 30;
    case kContextWidgetDelete: return 40;
    default: return 50;
    }
}

inline void Apply(HMENU root, std::vector<modern_menu::Item>& items,
    modern_menu::Appearance appearance = modern_menu::Appearance::FollowSystem)
{
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_MENUDATA;
    if (!GetMenuInfo(root, &info) || info.dwMenuData != kRootTag) return;
    struct Entry
    {
        modern_menu::Item item;
        Position position;
        unsigned originalBlock;
    };
    std::vector<modern_menu::Item> quick;
    std::vector<Entry> rows;
    const bool quickStrip = !modern_menu::appearance_rules::IsWin10Style(appearance);
    unsigned block = 0;
    for (auto& item : items)
    {
        if (item.separator) { ++block; continue; }
        if (quickStrip && item.quickAction && !item.inlineAction)
        { quick.push_back(std::move(item)); continue; }
        const auto position = Resolve(item);
        rows.push_back({std::move(item), position, block});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Entry& left, const Entry& right) {
        return std::tie(left.position.group, left.position.order) <
            std::tie(right.position.group, right.position.order);
    });
    std::stable_sort(quick.begin(), quick.end(), [](const auto& left, const auto& right) {
        return QuickOrder(left.command) < QuickOrder(right.command);
    });
    items = std::move(quick);
    for (size_t index = 0; index < rows.size(); ++index)
    {
        const auto& row = rows[index];
        const bool groupBoundary = index > 0 &&
            (row.position.group != rows[index - 1].position.group ||
                (row.position.group == Group::Custom &&
                    row.originalBlock != rows[index - 1].originalBlock));
        if (groupBoundary || (index == 0 && !items.empty()))
        {
            modern_menu::Item divider;
            divider.separator = true;
            items.push_back(std::move(divider));
        }
        items.push_back(std::move(rows[index].item));
    }
}
} // namespace snowdesktop::background_menu_order
