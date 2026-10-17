#pragma once

#include "common/types.h"
#include "collection_group_rules.h"

#include <unordered_set>

namespace snowdesktop::widget_group_drop
{
constexpr bool AcceptsChild(DesktopWidgetType group, DesktopWidgetType child)
{
    return group == DesktopWidgetType::CollectionGroup ? child == DesktopWidgetType::Collection
        : group == DesktopWidgetType::FileGroup &&
            (child == DesktopWidgetType::FileCategories || child == DesktopWidgetType::FolderMapping);
}

// One model transaction shared by desktop entrances, handles, Dock entries and
// group labels. Validate the entire membership change before detaching anything.
inline bool Apply(std::vector<DesktopWidget>& widgets, std::vector<DockEntry>& dock,
    size_t groupIndex, const std::vector<std::wstring>& requested, size_t insertIndex)
{
    if (groupIndex >= widgets.size() || widgets[groupIndex].id.empty() ||
        !IsGroupWidgetType(widgets[groupIndex].type) || requested.empty()) return false;
    const auto groupType = widgets[groupIndex].type;
    const auto uniqueIndex = [&](const std::wstring& id) {
        size_t result = widgets.size();
        for (size_t i = 0; i < widgets.size(); ++i)
            if (widgets[i].id == id)
            {
                if (result != widgets.size()) return widgets.size();
                result = i;
            }
        return result;
    };
    if (uniqueIndex(widgets[groupIndex].id) != groupIndex) return false;
    std::vector<std::wstring> moving;
    for (const auto& id : requested)
    {
        const size_t index = uniqueIndex(id);
        if (id.empty() || index >= widgets.size() || index == groupIndex ||
            !AcceptsChild(groupType, widgets[index].type)) return false;
        size_t owners = 0;
        for (const auto& widget : widgets)
            if (IsGroupWidgetType(widget.type))
                for (const auto& child : widget.childWidgetIds)
                    if (child == id && (++owners > 1 || widget.type != groupType)) return false;
        if (std::find(moving.begin(), moving.end(), id) == moving.end()) moving.push_back(id);
    }
    auto& target = widgets[groupIndex];
    const std::wstring active = target.activeCategoryId;
    insertIndex = std::min(insertIndex, target.childWidgetIds.size());
    const std::unordered_set<std::wstring> movingSet(moving.begin(), moving.end());
    size_t removedBefore = 0;
    for (size_t i = 0; i < insertIndex; ++i)
        if (movingSet.contains(target.childWidgetIds[i])) ++removedBefore;
    for (auto& widget : widgets)
    {
        if (widget.type != groupType) continue;
        std::erase_if(widget.childWidgetIds, [&](const auto& id) { return movingSet.contains(id); });
        widget.activeCategoryId = collection_group_rules::ResolveActiveItem(widget.childWidgetIds, widget.activeCategoryId);
    }
    const size_t insertAt = std::min(insertIndex - removedBefore, target.childWidgetIds.size());
    target.childWidgetIds.insert(target.childWidgetIds.begin() + static_cast<std::ptrdiff_t>(insertAt), moving.begin(), moving.end());
    target.activeCategoryId = collection_group_rules::ResolveActiveItem(target.childWidgetIds, active);
    std::erase_if(dock, [&](const auto& entry) {
        return (entry.type == DockEntryType::Collection || entry.type == DockEntryType::DesktopFiles ||
            entry.type == DockEntryType::FolderMapping) && movingSet.contains(entry.reference);
    });
    for (auto& widget : widgets)
        if (movingSet.contains(widget.id)) widget.selected = false;
    return true;
}
}
