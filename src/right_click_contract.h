#pragma once

#include "core/slot_contract.h"

#include <cstdint>

namespace snowdesktop::right_click_contract
{

enum class SlotItemKind : std::uint8_t
{
    None,
    DesktopItem,
    FolderEntry,
    CollectionGroupLabel,
    FileGroupLabel,
    Widget,
    LogicalSlotItem,
    Count,
};

enum class ContextMenuKind : std::uint8_t
{
    None,
    DesktopItem,
    ShellDesktopItem,
    FolderEntry,
    Widget,
    Dock,
    DockRunningApp,
    Background,
    FileGroupSourceTab,
    CollectionGroupTab,
    LogicalSlotItem,
};

enum class LuaWidgetMenuScope : std::uint8_t
{
    Widget,
    Element,
};

constexpr LuaWidgetMenuScope ResolveLuaWidgetMenuScope(
    bool hasElementAction) noexcept
{
    return hasElementAction
        ? LuaWidgetMenuScope::Element
        : LuaWidgetMenuScope::Widget;
}

template <typename MenuItems>
constexpr bool HasLuaElementMenuAction(const MenuItems& items) noexcept
{
    for (const auto& item : items)
    {
        if (!item.actionId.empty() && item.elementContext && !item.separator)
            return true;
        if (!item.children.empty() &&
            HasLuaElementMenuAction(item.children))
            return true;
    }
    return false;
}

constexpr ContextMenuKind ResolveSlotItemMenu(
    slot_contract::SlotSurfaceKind surface,
    SlotItemKind item,
    bool protectedDesktopItem)
{
    using Surface = slot_contract::SlotSurfaceKind;

    if (item == SlotItemKind::DesktopItem)
    {
        switch (surface)
        {
        case Surface::Desktop:
        case Surface::Dock:
        case Surface::Collection:
        case Surface::FileCategories:
        case Surface::CollectionGroup:
        case Surface::FileGroup:
            return protectedDesktopItem
                ? ContextMenuKind::ShellDesktopItem
                : ContextMenuKind::DesktopItem;
        default:
            return ContextMenuKind::None;
        }
    }

    if (item == SlotItemKind::FolderEntry)
    {
        switch (surface)
        {
        case Surface::Dock:
        case Surface::FolderMapping:
        case Surface::FileGroup:
            return ContextMenuKind::FolderEntry;
        default:
            return ContextMenuKind::None;
        }
    }

    if (item == SlotItemKind::Widget)
    {
        switch (surface)
        {
        case Surface::Desktop:
        case Surface::Dock:
        case Surface::CollectionGroup:
        case Surface::FileGroup:
            return ContextMenuKind::Widget;
        default:
            return ContextMenuKind::None;
        }
    }

    if (item == SlotItemKind::CollectionGroupLabel &&
        surface == Surface::CollectionGroup)
        return ContextMenuKind::CollectionGroupTab;

    if (item == SlotItemKind::FileGroupLabel &&
        surface == Surface::FileGroup)
        return ContextMenuKind::FileGroupSourceTab;

    if (item == SlotItemKind::LogicalSlotItem &&
        surface == Surface::LuaLogicalSlot)
        return ContextMenuKind::LogicalSlotItem;

    return ContextMenuKind::None;
}

constexpr ContextMenuKind ResolveContainerMenu(
    slot_contract::SlotSurfaceKind surface)
{
    using Surface = slot_contract::SlotSurfaceKind;
    switch (surface)
    {
    case Surface::Desktop:
        return ContextMenuKind::Background;
    case Surface::Dock:
        return ContextMenuKind::Dock;
    case Surface::Collection:
    case Surface::FileCategories:
    case Surface::FolderMapping:
    case Surface::CollectionGroup:
    case Surface::FileGroup:
    case Surface::LuaLogicalSlot:
    case Surface::Guide:
        return ContextMenuKind::Widget;
    default:
        return ContextMenuKind::None;
    }
}

constexpr bool ShouldPreserveSelectionOnRightClick(
    bool itemAlreadySelected)
{
    return itemAlreadySelected;
}

constexpr bool ShouldRestoreInteractionFocusAfterMenu(
    bool keepInteractionSurfaceOpen,
    bool inlineEditorStarted)
{
    return !keepInteractionSurfaceOpen &&
        !inlineEditorStarted;
}

} // namespace snowdesktop::right_click_contract
