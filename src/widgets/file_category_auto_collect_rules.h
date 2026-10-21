#pragma once

#include "common/types.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace snowdesktop::file_category_auto_collect_rules
{
// An empty selection means all categories, including categories added later.
inline bool Matches(const DesktopWidget& widget, std::wstring_view category)
{
    return widget.autoCollectCategoryIds.empty() ||
        std::find(widget.autoCollectCategoryIds.begin(),
            widget.autoCollectCategoryIds.end(), category) !=
            widget.autoCollectCategoryIds.end();
}

// Whether the component still auto-collects a member of the category. Members
// of categories that are no longer selected stay in the component but are no
// longer owned by its auto-collection, so they can be dragged out again.
inline bool AutoCollectsCategory(const DesktopWidget& widget, std::wstring_view category)
{
    return widget.autoCollect && Matches(widget, category);
}

// The collection menu behaves like a checkbox list in both scopes. Clearing
// one option while the empty All scope is active expands that scope first, so
// the clicked category is the one removed instead of the only one retained.
inline void ToggleCategory(DesktopWidget& widget, const std::wstring& category,
    const std::vector<std::wstring>& availableCategories)
{
    auto& selected = widget.autoCollectCategoryIds;
    if (!widget.autoCollect)
    {
        selected = {category};
        widget.autoCollect = true;
        return;
    }
    if (selected.empty())
    {
        for (const auto& available : availableCategories)
            if (available != L"all" && available != category)
                selected.push_back(available);
        if (selected.empty()) widget.autoCollect = false;
        return;
    }
    const auto found = std::find(selected.begin(), selected.end(), category);
    if (found == selected.end()) selected.push_back(category);
    else selected.erase(found);
    if (selected.empty()) widget.autoCollect = false;
}

// Whether another automatic collector owns the category. A category belongs to
// at most one collector, so items that no longer match this component's scope
// are handed to that owner instead of being evicted to the free desktop.
inline bool IsCategoryClaimedElsewhere(const std::vector<DesktopWidget>& widgets,
    size_t selfIndex, std::wstring_view category)
{
    for (size_t index = 0; index < widgets.size(); ++index)
    {
        if (index == selfIndex) continue;
        const auto& widget = widgets[index];
        if (widget.type != DesktopWidgetType::FileCategories ||
            !widget.autoCollect)
            continue;
        if (Matches(widget, category)) return true;
    }
    return false;
}

// Decides whether an item that is currently collected must leave the component.
// Items stay when the only change is this component's category scope; they are
// released only when they can no longer be collected at all or another
// component now owns their category.
inline bool ShouldReleaseCollectedItem(const DesktopWidget& widget,
    bool collectable, bool unclassifiedShortcut, std::wstring_view category,
    const std::vector<DesktopWidget>& widgets, size_t selfIndex)
{
    if (!collectable) return true;
    // Wait for the target classification of a new link; it is still a member.
    if (unclassifiedShortcut) return false;
    if (!widget.autoCollect) return false;
    if (Matches(widget, category)) return false;
    return IsCategoryClaimedElsewhere(widgets, selfIndex, category);
}

inline void ReleasePreviousSelection(DesktopWidget& widget)
{
    // Relinquish layout ownership only. Files stay on disk and can be collected
    // by their new owner; missing entries must not return to the old owner.
    widget.autoCollect = false;
    widget.itemKeys.clear();
    widget.missingItems.clear();
    widget.scrollOffset = 0;
    widget.activeCategoryId.clear();
}

// The latest selection owns each category. Expanding an earlier All selection
// into explicit IDs preserves its remaining categories without an overlap.
inline std::vector<size_t> EnforceExclusiveSelection(std::vector<DesktopWidget>& widgets,
    size_t activeIndex, const std::vector<std::wstring>& availableCategories)
{
    std::vector<size_t> changed;
    if (activeIndex >= widgets.size()) return changed;
    const auto& active = widgets[activeIndex];
    if (active.type != DesktopWidgetType::FileCategories || !active.autoCollect)
        return changed;
    for (size_t index = 0; index < widgets.size(); ++index)
    {
        auto& previous = widgets[index];
        if (index == activeIndex || !previous.autoCollect ||
            previous.type != DesktopWidgetType::FileCategories)
            continue;
        if (active.autoCollectCategoryIds.empty())
        {
            ReleasePreviousSelection(previous);
            changed.push_back(index);
            continue;
        }
        auto& selected = previous.autoCollectCategoryIds;
        const auto oldSelection = selected;
        if (selected.empty())
        {
            for (const auto& category : availableCategories)
                if (category != L"all") selected.push_back(category);
        }
        std::erase_if(selected, [&](const std::wstring& category) {
            return Matches(active, category);
        });
        if (selected.empty()) ReleasePreviousSelection(previous);
        if (!previous.autoCollect || selected != oldSelection)
            changed.push_back(index);
    }
    return changed;
}
}
