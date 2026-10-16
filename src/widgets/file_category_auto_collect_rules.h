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

inline void ToggleCategory(DesktopWidget& widget, const std::wstring& category)
{
    auto& selected = widget.autoCollectCategoryIds;
    if (!widget.autoCollect || selected.empty())
    {
        selected = {category};
        widget.autoCollect = true;
        return;
    }
    const auto found = std::find(selected.begin(), selected.end(), category);
    if (found == selected.end()) selected.push_back(category);
    else selected.erase(found);
    if (selected.empty()) widget.autoCollect = false;
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
