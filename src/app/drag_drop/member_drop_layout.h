#pragma once

#include "core/drop_model.h"
#include "core/widget.h"

namespace snowdesktop::member_drop_layout
{
// These views update their own member order and slot cache. Their footprint
// and membership on the desktop stay the same. Group proxies have their own
// presentation state and continue through the full layout path.
inline bool OnlyReordersMembers(const DragSourceList& sources, const DropPreviewList& preview)
{
    if (sources.hasSearchResults || preview.Empty() || preview.fileBacked ||
        preview.action != DropAction::Move || sources.origin != preview.targetContainer)
        return false;
    const auto* view = dynamic_cast<const WidgetContainer*>(preview.targetContainer);
    if (!view || !preview.targetWidget || view->GetWidgetData() != preview.targetWidget) return false;
    const auto type = preview.targetWidget->type;
    return type == DesktopWidgetType::Collection || type == DesktopWidgetType::FileCategories ||
        type == DesktopWidgetType::FolderMapping;
}
}
