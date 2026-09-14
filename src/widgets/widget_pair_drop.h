#pragma once

#include "../types.h"

#include <algorithm>
#include <utility>

namespace snowdesktop::widget_pair_drop
{
enum class Action
{
    None,
    Merge,
    CreateCollectionGroup,
    CreateFileGroup,
};

struct Options
{
    bool merge = false;
    Action group = Action::None;

    bool Available() const { return merge || group != Action::None; }
};

constexpr Options GetOptions(DesktopWidgetType source, DesktopWidgetType target)
{
    if (source == DesktopWidgetType::Collection &&
        target == DesktopWidgetType::Collection)
        return {true, Action::CreateCollectionGroup};
    if (source == DesktopWidgetType::FileCategories &&
        target == DesktopWidgetType::FileCategories)
        return {true, Action::CreateFileGroup};
    if ((source == DesktopWidgetType::FileCategories ||
         source == DesktopWidgetType::FolderMapping) &&
        (target == DesktopWidgetType::FileCategories ||
         target == DesktopWidgetType::FolderMapping))
        return {false, Action::CreateFileGroup};
    return {};
}

constexpr Action ResolveAction(Options options, bool control, bool shift, bool alt)
{
    // Ambiguous combinations never arm a merge or a new group.
    if (alt || control == shift) return Action::None;
    if (control) return options.group;
    return options.merge ? Action::Merge : Action::None;
}

inline bool CanPair(const std::vector<DesktopWidget>& widgets,
    size_t sourceIndex, size_t targetIndex)
{
    if (sourceIndex >= widgets.size() || targetIndex >= widgets.size() ||
        sourceIndex == targetIndex)
        return false;
    const auto& source = widgets[sourceIndex];
    const auto& target = widgets[targetIndex];
    if (source.id.empty() || target.id.empty() || source.id == target.id ||
        !GetOptions(source.type, target.type).Available())
        return false;
    // This gesture combines standalone components, never hidden group children.
    for (const auto& widget : widgets)
        for (const auto& child : widget.childWidgetIds)
            if (child == source.id || child == target.id) return false;
    return true;
}

// Production model transaction. No filesystem operation is involved: item keys
// retain their existing files, and grouping retains each child's full settings.
// Validate everything before changing ownership or removing the empty source.
template <typename NormalizeKey>
bool Apply(std::vector<DesktopWidget>& widgets, std::vector<DockEntry>& dock,
    size_t sourceIndex, size_t targetIndex, Action action,
    DesktopWidget group, NormalizeKey&& normalizeKey)
{
    if (!CanPair(widgets, sourceIndex, targetIndex)) return false;
    const auto options = GetOptions(widgets[sourceIndex].type, widgets[targetIndex].type);
    if (action == Action::None ||
        (action == Action::Merge ? !options.merge : action != options.group))
        return false;
    const std::wstring sourceId = widgets[sourceIndex].id;
    const std::wstring targetId = widgets[targetIndex].id;
    if (action == Action::Merge)
    {
        auto keys = widgets[targetIndex].itemKeys;
        std::vector<std::wstring> claimed;
        for (const auto& key : keys) claimed.push_back(normalizeKey(key));
        for (const auto& key : widgets[sourceIndex].itemKeys)
        {
            const auto normalized = normalizeKey(key);
            if (std::find(claimed.begin(), claimed.end(), normalized) == claimed.end())
            {
                keys.push_back(key);
                claimed.push_back(normalized);
            }
        }
        widgets[targetIndex].itemKeys = std::move(keys);
        widgets[sourceIndex].itemKeys.clear();
        widgets.erase(widgets.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
    }
    else
    {
        if (group.id.empty() || std::any_of(widgets.begin(), widgets.end(),
                [&](const auto& widget) { return widget.id == group.id; }))
            return false;
        group.type = action == Action::CreateCollectionGroup
            ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileGroup;
        group.childWidgetIds = {targetId, sourceId};
        group.activeCategoryId = targetId;
        group.dissolveWhenSingle = true;
        widgets.push_back(std::move(group));
        widgets[sourceIndex].selected = false;
        widgets[targetIndex].selected = false;
    }
    std::erase_if(dock, [&](const DockEntry& entry) {
        return IsWidgetDockEntryType(entry.type) &&
            (entry.reference == sourceId ||
             (action != Action::Merge && entry.reference == targetId));
    });
    return true;
}

inline bool ShouldDissolve(const DesktopWidget& group)
{
    return group.dissolveWhenSingle &&
        (group.type == DesktopWidgetType::CollectionGroup ||
         group.type == DesktopWidgetType::FileGroup) &&
        group.childWidgetIds.size() <= 1;
}

// The host plans a free landing before this ownership transaction. Keep the
// original child settings, inherit the wrapper's current size, and remove the wrapper.
inline bool Dissolve(std::vector<DesktopWidget>& widgets, size_t groupIndex,
    GridCell landing)
{
    if (groupIndex >= widgets.size() || !ShouldDissolve(widgets[groupIndex]))
        return false;
    const auto& group = widgets[groupIndex];
    if (!group.childWidgetIds.empty())
    {
        const auto child = std::find_if(widgets.begin(), widgets.end(),
            [&](const auto& widget) { return widget.id == group.childWidgetIds.front(); });
        if (child == widgets.end() || child == widgets.begin() + groupIndex)
            return false;
        const auto expected = GetOptions(child->type, child->type).group;
        if ((group.type == DesktopWidgetType::CollectionGroup &&
             expected != Action::CreateCollectionGroup) ||
            (group.type == DesktopWidgetType::FileGroup && expected != Action::CreateFileGroup))
            return false;
        for (size_t i = 0; i < widgets.size(); ++i)
            if (i != groupIndex && std::find(widgets[i].childWidgetIds.begin(),
                    widgets[i].childWidgetIds.end(), child->id) != widgets[i].childWidgetIds.end())
                return false;
        child->gridCell = std::move(landing);
        child->gridSpan = group.gridSpan;
    }
    widgets.erase(widgets.begin() + static_cast<std::ptrdiff_t>(groupIndex));
    return true;
}
}
