#pragma once

#include "core/drop_model.h"
#include "widgets/widget_pair_drop.h"

namespace snowdesktop::widget_pair_drag
{
struct Source
{
    size_t widgetIndex = static_cast<size_t>(-1);
    std::wstring groupId;
};

// Resolve captured IDs instead of the active tab or runtime Item pointers:
// dwell switching and container rebuilds must not change the dragged child.
inline Source ResolveSource(const DragSourceList& list,
    const std::vector<DesktopWidget>& widgets, const std::vector<DockEntry>& dock)
{
    if (list.entries.size() != 1) return {};
    const auto& entry = list.entries.front();
    const auto findWidget = [&](const std::wstring& id) {
        const auto found = std::find_if(widgets.begin(), widgets.end(),
            [&](const auto& widget) { return widget.id == id; });
        return static_cast<size_t>(std::distance(widgets.begin(), found));
    };
    if (entry.fromDock && entry.kind == DropSourceKind::Widget)
    {
        const size_t index = findWidget(entry.dockReference);
        if (index >= widgets.size() || !IsWidgetDockEntryType(entry.dockEntryType) ||
            DockEntryTypeForWidget(widgets[index].type) != entry.dockEntryType ||
            !widget_pair_drop::CanUseSource(widgets, index) ||
            std::none_of(dock.begin(), dock.end(), [&](const auto& item) {
                return item.type == entry.dockEntryType && item.reference == entry.dockReference;
            }))
            return {};
        return {index, {}};
    }
    if (entry.fromDock || !list.hasOriginWidget || list.originWidgetId.empty())
        return {};
    const size_t index = findWidget(entry.widgetId);
    if (index >= widgets.size()) return {};
    const auto type = widgets[index].type;
    const bool collectionLabel = list.hasCollectionGroupEntries &&
        entry.kind == DropSourceKind::CollectionGroupEntry &&
        list.originWidgetType == DesktopWidgetType::CollectionGroup &&
        type == DesktopWidgetType::Collection;
    const bool fileLabel = list.hasFileGroupSourceLabels &&
        list.originWidgetType == DesktopWidgetType::FileGroup &&
        ((list.hasFileGroupEntries && entry.kind == DropSourceKind::FileGroupEntry &&
          type == DesktopWidgetType::FileCategories) ||
         (list.hasWidgets && entry.kind == DropSourceKind::Widget &&
          entry.dockEntryType == DockEntryType::FolderMapping &&
          type == DesktopWidgetType::FolderMapping));
    if ((!collectionLabel && !fileLabel) ||
        !widget_pair_drop::CanUseSource(widgets, index, list.originWidgetId))
        return {};
    return {index, list.originWidgetId};
}
}
