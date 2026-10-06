#pragma once

#include "types.h"
#include <algorithm>
#include <vector>

namespace snowdesktop::empty_group_drop_rules
{
inline bool IsEmptyGroup(const DesktopWidget& group,
    const std::vector<DesktopWidget>& widgets)
{
    const bool collections = group.type == DesktopWidgetType::CollectionGroup;
    if ((!collections && group.type != DesktopWidgetType::FileGroup) ||
        !group.itemKeys.empty() || !group.folderEntries.empty()) return false;
    return std::none_of(group.childWidgetIds.begin(), group.childWidgetIds.end(),
        [&](const std::wstring& id) {
            return std::any_of(widgets.begin(), widgets.end(), [&](const DesktopWidget& child) {
                return child.id == id && (collections
                    ? child.type == DesktopWidgetType::Collection
                    : child.type == DesktopWidgetType::FileCategories ||
                        child.type == DesktopWidgetType::FolderMapping);
            });
        });
}

// This is an in-place type change: identity, page, span, bounds and display
// preferences stay with the component. Valid child containers are never lost.
inline bool Convert(DesktopWidget& group, const std::vector<DesktopWidget>& widgets,
    bool programs, const std::wstring& defaultTitle)
{
    if (!IsEmptyGroup(group, widgets)) return false;
    group.type = programs ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
    if (!group.userRenamed) group.title = defaultTitle;
    group.childWidgetIds.clear();
    group.activeCategoryId.clear();
    group.dissolveWhenSingle = false;
    return true;
}
}
