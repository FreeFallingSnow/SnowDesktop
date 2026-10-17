#include "app/app.h"
#include "widgets/collection_group_rules.h"
#include "desktop/desktop_source.h"
#include "dock/dock_drop_model.h"
#include "dock/dock_desktop_return.h"

// Dock insertion, collection drops and restoration to the desktop.

snowdesktop::dock_drop_model::Services DesktopApp::DockDropServices()
{
    snowdesktop::dock_drop_model::Services services;
    services.normalize = ToUpperInvariant;
    services.recycle = [this](const auto& entry) { return IsRecycleBinDockEntry(entry); };
    services.folder = [this](const auto& entry) { return IsFolderDockEntry(entry); };
    services.item = [this](const auto& key) -> DesktopItem* {
        const auto index = FindItemIndexByKey(key);
        return index < items_.size() ? &items_[index] : nullptr;
    };
    services.capacity = [this](size_t count) {
        bool found = false;
        for (const auto& container : containers_)
        {
            const auto* dock = dynamic_cast<DockContainer*>(container.get());
            if (!dock) continue;
            found = true;
            if (!dock->HasCapacity(count)) return false;
        }
        return found;
    };
    return services;
}


void DesktopApp::CommitDockDrop(const std::vector<Item*>& sourceItems,
    Container* origin, DockContainer* targetDock, size_t insertIndex, int mods)
{
    if (!targetDock || sourceItems.empty()) return;
    if ((mods & (MK_CONTROL | MK_ALT)) != 0)
    {
        std::vector<Item*> files;
        for (Item* source : sourceItems)
        {
            const auto* widget = dynamic_cast<const Widget*>(source);
            const auto* data = widget ? widget->GetWidgetData() : nullptr;
            if (!data || !snowdesktop::single_cell_widget::IsIconMode(*data)) files.push_back(source);
        }
        if (files.size() != sourceItems.size())
        {
            if (!files.empty()) CommitDockDrop(files, origin, targetDock, insertIndex, mods);
            return;
        }
    }

    if (dynamic_cast<DockContainer*>(origin))
    {
        std::vector<size_t> indices;
        for (Item* item : sourceItems)
            if (auto* dockItem = dynamic_cast<DockEntryItem*>(item))
            {
                const size_t index = dockItem->GetEntryIndex();
                if (index < dockEntries_.size() &&
                    !IsRecycleBinDockEntry(dockEntries_[index]))
                    indices.push_back(index);
            }
        const auto result = snowdesktop::dock_drop_model::Reorder(dockEntries_, std::move(indices), insertIndex, DockDropServices());
        if (result != snowdesktop::dock_drop_model::ReorderResult::Changed)
        {
            if (result == snowdesktop::dock_drop_model::ReorderResult::MixedSections) MessageBeep(MB_ICONWARNING);
            return;
        }
        InvalidateDockContainers();
        return;
    }

    const bool folderEntriesOnly =
        std::all_of(
            sourceItems.begin(), sourceItems.end(),
            [](Item* source) {
                return dynamic_cast<FolderEntryIcon*>(
                    source) != nullptr;
            });
    if (folderEntriesOnly)
    {
        if (!targetDock->HasCapacity(sourceItems.size()))
        {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        DragSourceList sourceList =
            BuildDragSourceList(sourceItems, origin);
        DropPreviewList preview =
            BuildDropPreviewList(
                sourceList, GetDesktopGrid(),
                nullptr, HitRegion::Empty,
                mods, dragSession_.CurrentPoint());
        preview.action = DropAction::Link;
        preview.pinMaterializedItemsToDock = true;
        preview.dockInsertIndex = insertIndex;
        ExecuteDropPipeline(sourceList, preview);
        return;
    }

    const bool keepSource = (mods & MK_CONTROL) != 0;
    std::vector<DockEntry> additions;
    for (Item* source : sourceItems)
    {
        if (auto* icon = dynamic_cast<DesktopIcon*>(source))
        {
            DesktopItem* item = icon->GetDesktopItem();
            if (!item || item->layoutKey.empty()) continue;
            additions.push_back({ DockEntryType::DesktopItem,
                ToUpperInvariant(item->layoutKey), keepSource });
            continue;
        }

        auto* widget = dynamic_cast<Widget*>(source);
        DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
        if (data && IsWidgetDockEntryType(DockEntryTypeForWidget(data->type)))
            additions.push_back({ DockEntryTypeForWidget(data->type), data->id, false });
        else if (auto* collectionEntry = dynamic_cast<CollectionGroupEntryItem*>(source))
        {
            const size_t child = FindWidgetIndexById(collectionEntry->GetCollectionId());
            if (child >= widgets_.size() || widgets_[child].type != DesktopWidgetType::Collection) return;
            additions.push_back({ DockEntryType::Collection, widgets_[child].id, false });
        }
        else if (auto* groupEntry =
                     dynamic_cast<FileGroupEntryItem*>(source))
        {
            const size_t widgetIndex =
                FindWidgetIndexById(
                    groupEntry->GetChildWidgetId());
            if (widgetIndex < widgets_.size() &&
                (widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping ||
                 widgets_[widgetIndex].type == DesktopWidgetType::FileCategories))
            {
                additions.push_back({
                    DockEntryTypeForWidget(widgets_[widgetIndex].type),
                    widgets_[widgetIndex].id,
                    false });
            }
            else return;
        }
        else return;
    }
    if (additions.empty()) return;

    auto services = DockDropServices();
    services.capacity = [targetDock](size_t count) { return targetDock->HasCapacity(count); };
    if (!snowdesktop::dock_drop_model::Pin({dockEntries_, widgets_}, additions, insertIndex, services))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    RefreshCollectedKeysCache();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
}

bool DesktopApp::AddMaterializedItemsToDock(
    const std::vector<std::wstring>& createdPaths,
    size_t insertIndex, bool keepOnDesktop)
{
    if (!snowdesktop::dock_drop_model::AddMaterialized({dockEntries_, widgets_}, createdPaths,
            insertIndex, keepOnDesktop, DockDropServices())) return false;
    RefreshCollectedKeysCache();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    return true;
}

bool DesktopApp::CanPinExistingDesktopPaths(const std::vector<std::wstring>& paths) const
{
    std::vector<std::filesystem::path> roots;
    if (snowdesktop::debug_profile::Enabled())
        roots.emplace_back(snowdesktop::desktop_source::Directory());
    else
        roots = snowdesktop::desktop_source::SystemDesktops();
    return snowdesktop::dock_drop_rules::CanReferenceDesktopPaths(paths, roots);
}

bool DesktopApp::FindDockReturnCell(
    std::unordered_set<std::wstring>& usedSlots,
    const std::wstring& preferredPageId, int startSlot, GridSpan span,
    GridCell& result)
{
    span.columns = std::max(1, span.columns);
    span.rows = std::max(1, span.rows);
    if (TryFindFreeCell(span, usedSlots, result, preferredPageId, startSlot))
        return true;

    // Try saved pages that are currently off-screen before allocating a new one.
    for (const std::wstring& pageId : savedPageIds_)
    {
        auto columnsIt = savedPageColumns_.find(pageId);
        auto rowsIt = savedPageRows_.find(pageId);
        if (columnsIt == savedPageColumns_.end() || rowsIt == savedPageRows_.end()) continue;
        const int columns = std::max(1, columnsIt->second);
        const int rows = std::max(1, rowsIt->second);
        for (int slot = 0; slot < columns * rows; ++slot)
        {
            GridCell candidate{ pageId, slot / rows, slot % rows };
            if (candidate.column + span.columns <= columns &&
                candidate.row + span.rows <= rows &&
                !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                result = candidate;
                return true;
            }
        }
    }

    if (gridPages_.empty()) return false;
    const std::vector<size_t> order = BuildMonitorRenderOrder();
    const GridPage& reference = gridPages_[order.empty() ? 0 : order.back()];
    const std::wstring pageId = GeneratePageId();
    RememberSavedPageId(pageId);
    savedPageColumns_[pageId] = std::max(span.columns, reference.columns);
    savedPageRows_[pageId] = std::max(span.rows, reference.rows);
    result = { pageId, 0, 0 };
    return true;
}

bool DesktopApp::DropItemsIntoDockCollection(
    const std::vector<Item*>& sourceItems, Container* origin,
    DockEntryItem* targetItem, int mods)
{
    if (!targetItem || !IsLogicalDockEntryType(targetItem->GetEntryType()))
        return false;
    size_t widgetIndex = FindWidgetIndexById(targetItem->GetReference());
    if (widgetIndex < widgets_.size() && IsGroupWidgetType(widgets_[widgetIndex].type))
    {
        const DragSourceList sourceList = BuildDragSourceList(sourceItems, origin);
        if (sourceList.HasDesktopWidgetIcons())
        {
            for (const auto& container : containers_)
                if (auto* group = dynamic_cast<WidgetContainer*>(container.get());
                    group && group->GetWidgetData() == &widgets_[widgetIndex])
                    return DropDesktopWidgetIconsIntoGroup(group, sourceList, nullptr, HitRegion::Empty, mods);
            return false;
        }
        const auto payload = sourceList.SlotPayloadKind();
        if (payload == snowdesktop::slot_contract::DragPayloadKind::CollectionWidget ||
            payload == snowdesktop::slot_contract::DragPayloadKind::CollectionGroupLabel ||
            sourceList.UsesFileGroupSourceInsertion())
            return MoveWidgetSourcesToGroup(sourceItems, widgetIndex, widgets_[widgetIndex].childWidgetIds.size());
        for (const auto& container : containers_)
        {
            auto* group = dynamic_cast<WidgetContainer*>(container.get());
            if (!group || group->GetWidgetData() != &widgets_[widgetIndex]) continue;
            const auto preview = BuildDropPreviewList(sourceList, group, nullptr,
                HitRegion::Empty, mods, dragSession_.CurrentPoint());
            return ExecuteDropPipeline(sourceList, preview);
        }
        return false;
    }
    if (widgetIndex >= widgets_.size() ||
        (widgets_[widgetIndex].type != DesktopWidgetType::Collection &&
         widgets_[widgetIndex].type != DesktopWidgetType::FileCategories))
        return false;

    std::unique_ptr<WidgetContainer> collection;
    if (widgets_[widgetIndex].type == DesktopWidgetType::FileCategories)
        collection = std::make_unique<FileCategories>(&widgets_[widgetIndex], this);
    else
        collection = std::make_unique<Collection>(&widgets_[widgetIndex], this);
    DragSourceList sourceList = BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = BuildDropPreviewList(sourceList, collection.get(),
        nullptr, HitRegion::SortAfter, mods, dragSession_.CurrentPoint());
    return ExecuteDropPipeline(sourceList, preview);
}

snowdesktop::dock_desktop_return::Services DesktopApp::DockDesktopReturnServices() const
{
    snowdesktop::dock_desktop_return::Services services;
    services.normalize = ToUpperInvariant;
    services.grouped = [this](const auto& widget) { return IsGroupedWidget(widget); };
    services.owned = [this](const auto& item) { return IsItemInAnyWidget(item); };
    services.itemIndex = [this](const auto& key) { return FindItemIndexByKey(key); };
    return services;
}

std::vector<snowdesktop::dock_desktop_return::Landing> DesktopApp::PlanDockDesktopReturn(
    const std::vector<Item*>& sourceItems, GridCell target) const
{
    std::vector<size_t> indices;
    for (Item* source : sourceItems)
        if (auto* item = dynamic_cast<DockEntryItem*>(source)) indices.push_back(item->GetEntryIndex());
    return snowdesktop::dock_desktop_return::Plan({dockEntries_, widgets_, items_, gridPages_},
        std::move(indices), std::move(target), DockDesktopReturnServices());
}

void DesktopApp::MoveDockItemsToDesktop(
    const std::vector<Item*>& sourceItems, GridCell targetCell)
{
    const auto landings = PlanDockDesktopReturn(sourceItems, std::move(targetCell));
    if (!snowdesktop::dock_desktop_return::Apply(dockEntries_, widgets_, items_, landings, DockDesktopReturnServices())) return;
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    RebuildContainersAndItems();
    LayoutItems();
    InvalidateDragStaticScene();
}

void DesktopApp::RestoreDockEntriesToDesktop()
{
    if (dockEntries_.empty()) return;
    const GridPage* first = GetFirstPageGridPage();
    const std::wstring preferredPage = first ? first->id : L"";
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (!IsGroupedWidget(widget) &&
            widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    int startSlot = 0;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (IsWidgetDockEntryType(entry.type))
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        GridCell cell;
        if (!FindDockReturnCell(usedSlots, preferredPage, startSlot, span, cell)) continue;
        MarkGridArea(usedSlots, cell, span);
        ++startSlot;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = cell;
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = cell;
        }
    }
    dockEntries_.clear();
    RefreshCollectedKeysCache();
}
