#pragma once

#include "core/drop_model.h"

namespace snowdesktop::desktop_widget_icon_drop
{
inline bool Accepts(const DragSourceEntry& entry, slot_contract::SlotSurfaceKind target)
{
    if (!entry.desktopWidgetIcon || entry.fromDock || entry.kind != DropSourceKind::Widget ||
        entry.widgetId.empty() || !entry.widgetType ||
        static_cast<size_t>(*entry.widgetType) >= static_cast<size_t>(DesktopWidgetType::Count))
        return false;
    if (*entry.widgetType != DesktopWidgetType::Collection && *entry.widgetType != DesktopWidgetType::FileCategories &&
        *entry.widgetType != DesktopWidgetType::FolderMapping && !IsGroupWidgetType(*entry.widgetType)) return false;
    return slot_contract::AcceptsSlotDrop(slot_contract::SlotSurfaceKind::Desktop,
        slot_contract::PayloadForWidgetType(*entry.widgetType), target,
        slot_contract::DragRelation::CrossSurface);
}

// Native entrances are identities, not copyable filesystem objects. Unsupported
// members stay at the source while ordinary files keep their captured indices.
inline DragSourceList Filter(const DragSourceList& source,
    slot_contract::SlotSurfaceKind target, int mods = 0)
{
    DragSourceList result = source;
    const bool move = (mods & (MK_CONTROL | MK_ALT)) == 0;
    std::erase_if(result.entries, [&](const auto& entry) {
        return entry.desktopWidgetIcon && (!move || !Accepts(entry, target));
    });
    result.hasWidgets = std::any_of(result.entries.begin(), result.entries.end(),
        [](const auto& entry) { return entry.kind == DropSourceKind::Widget; });
    return result;
}

inline bool HasGroupMembers(const DragSourceList& source,
    DesktopWidgetType group, int mods = 0)
{
    if (!IsGroupWidgetType(group) || (mods & (MK_CONTROL | MK_ALT)) != 0) return false;
    const auto surface = slot_contract::SurfaceForWidgetType(group);
    return std::any_of(source.entries.begin(), source.entries.end(),
        [&](const auto& entry) { return Accepts(entry, surface); });
}
}
