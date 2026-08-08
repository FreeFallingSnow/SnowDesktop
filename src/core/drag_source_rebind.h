#pragma once

#include "drop_model.h"

#include <vector>

namespace snowdesktop::drag_source_rebind
{
inline bool CanRestoreRecordedWidgetMembers(
    const DragSourceList& source)
{
    return source.hasOriginWidget &&
        (source.originWidgetType ==
             DesktopWidgetType::Collection ||
         source.originWidgetType ==
             DesktopWidgetType::FolderMapping);
}

/**
 * @brief Resolve runtime drag items after a container-tree rebuild.
 *
 * Most sources can enumerate their selected runtime wrappers directly.  A
 * Collection or FolderMapping on the page replaced by a same-monitor turn has
 * no visible slots, so recreate only the members recorded when the drag began.
 */
template <typename ResolveMember>
std::vector<Item*> ResolveItemsAfterRebuild(
    std::vector<Item*> runtimeItems,
    const DragSourceList& source,
    ResolveMember&& resolveMember)
{
    if (!runtimeItems.empty() ||
        !CanRestoreRecordedWidgetMembers(source))
    {
        return runtimeItems;
    }

    runtimeItems.reserve(source.entries.size());
    for (const DragSourceEntry& entry : source.entries)
    {
        if (entry.memberIndex ==
            static_cast<size_t>(-1))
        {
            runtimeItems.clear();
            return runtimeItems;
        }
        Item* item = resolveMember(
            entry.memberIndex);
        if (!item)
        {
            runtimeItems.clear();
            return runtimeItems;
        }
        runtimeItems.push_back(item);
    }
    return runtimeItems;
}
}
