#include "drop_model.h"

#include "container.h"
#include "item.h"
#include "widget.h"
#include "common/utils.h"

DropPayload DropPayload::From(const std::vector<Item*>& sourceItems)
{
    DropPayload payload;
    payload.items = sourceItems;
    for (auto* src : sourceItems)
    {
        if (!src) continue;

        if (dynamic_cast<Widget*>(src))
        {
            payload.hasWidgets = true;
            continue;
        }

        if (auto* icon = dynamic_cast<DesktopIcon*>(src))
        {
            payload.hasDesktopIcons = true;
            DesktopItem* item = icon->GetDesktopItem();
            if (item && !item->layoutKey.empty())
                payload.desktopKeys.push_back(ToUpperInvariant(item->layoutKey));

            std::wstring path = icon->GetPath();
            if (item && item->desktopIconClsid.empty() && !path.empty())
                payload.filePaths.push_back(path);
            continue;
        }

        if (dynamic_cast<FolderEntryIcon*>(src))
            payload.hasFolderEntries = true;
        else if (dynamic_cast<ExternalFileItem*>(src))
            payload.hasExternalFiles = true;

        std::wstring path = src->GetPath();
        if (!path.empty())
            payload.filePaths.push_back(path);
    }
    return payload;
}

void DragSourceList::BindRuntimeOrigin(Container* value)
{
    origin = value;
    if (!value) return;
    originSurface = hasSearchResults
        ? snowdesktop::slot_contract::SlotSurfaceKind::SearchResults
        : value->GetSlotSurfaceKind();
    hasOriginSurface = true;
}

snowdesktop::slot_contract::DragPayloadKind DragSourceList::SlotPayloadKind() const
{
    if (hasSearchResults)
        return !hasWidgets && !hasCollectionGroupEntries && !hasFileGroupEntries
            ? snowdesktop::slot_contract::DragPayloadKind::SearchResult
            : snowdesktop::slot_contract::DragPayloadKind::Count;
    bool collectionWidgetsOnly = false;
    bool folderMappingWidgetsOnly = false;
    bool fileSourceWidgetsOnly = false;
    bool groupWidgetsOnly = false;
    if (hasWidgets)
    {
        collectionWidgetsOnly = true;
        folderMappingWidgetsOnly = true;
        fileSourceWidgetsOnly = true;
        groupWidgetsOnly = true;
        bool sawWidget = false;
        for (const auto& entry : entries)
        {
            if (entry.kind != DropSourceKind::Widget)
                continue;
            sawWidget = true;
            if (entry.fromDock)
            {
                groupWidgetsOnly = groupWidgetsOnly && IsGroupDockEntryType(entry.dockEntryType);
                collectionWidgetsOnly = collectionWidgetsOnly &&
                    entry.dockEntryType == DockEntryType::Collection;
                folderMappingWidgetsOnly = folderMappingWidgetsOnly &&
                    entry.dockEntryType == DockEntryType::FolderMapping;
                fileSourceWidgetsOnly = fileSourceWidgetsOnly &&
                    (entry.dockEntryType == DockEntryType::FolderMapping ||
                     entry.dockEntryType == DockEntryType::DesktopFiles);
                continue;
            }
            if (entry.dockEntryType == DockEntryType::FolderMapping)
            {
                collectionWidgetsOnly = false;
                groupWidgetsOnly = false;
                continue;
            }
            auto* widget = dynamic_cast<Widget*>(entry.item);
            DesktopWidget* data =
                widget ? widget->GetWidgetData() : nullptr;
            const auto type = entry.widgetType ? entry.widgetType : data
                ? std::optional<DesktopWidgetType>(data->type) : std::nullopt;
            groupWidgetsOnly = groupWidgetsOnly && type && IsGroupWidgetType(*type);
            collectionWidgetsOnly = collectionWidgetsOnly && type &&
                *type == DesktopWidgetType::Collection;
            folderMappingWidgetsOnly =
                folderMappingWidgetsOnly && type &&
                *type == DesktopWidgetType::FolderMapping;
            fileSourceWidgetsOnly = fileSourceWidgetsOnly && type &&
                (*type == DesktopWidgetType::FolderMapping ||
                 *type == DesktopWidgetType::FileCategories);
        }
        collectionWidgetsOnly = sawWidget && collectionWidgetsOnly;
        folderMappingWidgetsOnly =
            sawWidget && folderMappingWidgetsOnly;
        fileSourceWidgetsOnly = sawWidget && fileSourceWidgetsOnly;
        groupWidgetsOnly = sawWidget && groupWidgetsOnly;
    }
    return snowdesktop::slot_contract::ClassifyPayload({
        hasDesktopIcons,
        hasFolderEntries,
        hasExternalFiles,
        hasWidgets,
        collectionWidgetsOnly,
        hasCollectionGroupEntries,
        hasFileGroupEntries,
        folderMappingWidgetsOnly,
        fileSourceWidgetsOnly,
        groupWidgetsOnly,
    });
}
