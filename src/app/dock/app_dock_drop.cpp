#include "app/app.h"
#include "common/utils.h"
#include "widgets/collection_group_rules.h"
#include "desktop/desktop_source.h"
#include "dock/dock_drop_model.h"
#include "dock/dock_desktop_return.h"
#include "settings/settings_controller.h"
#include "settings/settings_snapshot.h"

// Dock insertion, collection drops and restoration to the desktop.

bool DockContainer::PreviewFreeOrderForDrag(DesktopApp& app)
{
    auto& session = app.dragSession_;
    if (!session.IsActive() || app.dockSettings_.freeItemOrder || !app.settingsController_ ||
        app.dockRunningDrag_) return false;

    const auto* dock = dynamic_cast<const DockContainer*>(session.TargetContainer());
    const HitRegion region = session.TargetRegion();
    if (!dock || (region != HitRegion::SortBefore && region != HitRegion::SortAfter && region != HitRegion::Empty) ||
        (session.Source() != dock && !dock->HasCapacity(std::max<size_t>(1, session.Items().size()))))
        return false;
    if (app.dragDropController_.IsExternalDragActive() &&
        app.dragDropController_.ExternalSummary().applicationShortcut) return false;

    const POINT point = session.CurrentPoint();
    const bool overFiles = dock->GetSectionAtPoint(point) == DockEntrySection::Files;
    const bool belongsToFiles = dock->HasOnlyFolderDragSource();
    if (overFiles == belongsToFiles && !dock->HasMixedFolderDragSource()) return false;

    const bool hadQuickSearch = std::any_of(app.dockEntries_.begin(), app.dockEntries_.end(),
        [](const auto& entry) { return entry.type == DockEntryType::QuickSearch; });
    session.SetDockOrderPreview(hadQuickSearch ? DragSession::DockFreeOrderPreview::ExistingQuickSearch
                                              : DragSession::DockFreeOrderPreview::AddedQuickSearch);
    // Preview the same layout as the switch, while leaving its persisted
    // value untouched until a Dock insertion actually accepts the drop.
    app.dockSettings_.freeItemOrder = true;
    app.ResetDockHandoffDwell();
    app.NormalizeDockRecycleBinPosition();
    app.RebuildContainersAndItems();
    if (session.IsActive())
    {
        app.ResolveCurrentDragTargetAt(point);
        app.ShowDragHintWindow(point, app.GetCurrentDragTargetHint(DropActionToMods(session.Action())));
        if (app.dragDropController_.IsTransportActive()) app.PresentOleDragInteractionFrame();
        else app.PresentPointerInteractionFrame();
    }
    return true;
}

void DockContainer::FinishFreeOrderForDrag(DesktopApp& app, bool dropped)
{
    auto& session = app.dragSession_;
    const auto preview = session.DockOrderPreview();
    if (preview == DragSession::DockFreeOrderPreview::None) return;
    session.SetDockOrderPreview(DragSession::DockFreeOrderPreview::None);

    if (app.settingsController_)
    {
        auto settings = app.settingsController_->Snapshot()->values.dock;
        if (dropped && !settings.freeItemOrder)
        {
            settings.freeItemOrder = true;
            app.settingsController_->UpdateDock(std::move(settings), snowdesktop::SettingsUpdateMode::Commit);
        }
        if (app.settingsController_->Snapshot()->values.dock.freeItemOrder) return;
    }

    app.dockSettings_.freeItemOrder = false;
    if (preview == DragSession::DockFreeOrderPreview::AddedQuickSearch)
        std::erase_if(app.dockEntries_, [](const auto& entry) { return entry.type == DockEntryType::QuickSearch; });
    app.NormalizeDockRecycleBinPosition();
    app.InvalidateDockContainers();
    app.InvalidateDragStaticScene();
}

HitRegion DockContainer::ResolveFreeOrderPreviewTarget(POINT point, Slot*& slot, HitRegion region)
{
    if (region != HitRegion::Handoff || app_->dragSession_.DockOrderPreview() == DragSession::DockFreeOrderPreview::None)
        return region;
    app_->ResetDockHandoffDwell();
    return RedirectDragToNearestInsertion(point, slot);
}

snowdesktop::dock_drop_model::Services DesktopApp::DockDropServices()
{
    snowdesktop::dock_drop_model::Services services;
    services.freeItemOrder = dockSettings_.freeItemOrder;
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


DropReceipt DesktopApp::CommitDockDrop(const std::vector<Item*>& sourceItems,
    Container* origin, DockContainer* targetDock, size_t insertIndex, int mods, DWORD allowed,
    std::optional<DockEntrySection> section)
{
    if (!targetDock || sourceItems.empty()) return {};
    auto services = DockDropServices();
    if (dockSettings_.freeItemOrder) services.dropSection = section.value_or(targetDock->GetSectionAtPoint(lastMousePoint_));
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
            if (!files.empty()) return CommitDockDrop(files, origin, targetDock, insertIndex, mods, allowed, section);
            return {};
        }
    }

    const bool fromDock = dynamic_cast<DockContainer*>(origin) != nullptr;
    const bool pinFrequent = fromDock && std::all_of(sourceItems.begin(), sourceItems.end(),
        [](const Item* item) { return dynamic_cast<const DockFrequentItem*>(item) != nullptr; });
    if (fromDock && !pinFrequent)
    {
        if (!(allowed & DROPEFFECT_MOVE)) return {};
        std::vector<size_t> indices;
        for (Item* item : sourceItems)
            if (auto* dockItem = dynamic_cast<DockEntryItem*>(item))
            {
                const size_t index = dockItem->GetEntryIndex();
                if (index < dockEntries_.size()) indices.push_back(index);
            }
        const auto result = snowdesktop::dock_drop_model::Reorder(dockEntries_, std::move(indices), insertIndex, services, true);
        if (result != snowdesktop::dock_drop_model::ReorderResult::Changed)
        {
            if (result == snowdesktop::dock_drop_model::ReorderResult::MixedSections) MessageBeep(MB_ICONWARNING);
            return {};
        }
        InvalidateDockContainers();
        return {DropAction::Move};
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
        if (!(allowed & DROPEFFECT_LINK)) return {};
        if (!targetDock->HasCapacity(sourceItems.size()))
        {
            MessageBeep(MB_ICONWARNING);
            return {};
        }
        DragSourceList sourceList =
            BuildDragSourceList(sourceItems, origin);
        const auto preview = targetDock->FilePinPreview(sourceList, insertIndex, dragSession_.CurrentPoint());
        return DropReceipt::Submitted(ExecuteDropPipeline(sourceList, preview), DropAction::Link);
    }

    const bool keepSource = pinFrequent || (mods & MK_CONTROL) != 0;
    std::vector<DockEntry> additions;
    for (Item* source : sourceItems)
    {
        if (pinFrequent)
        {
            const size_t index = static_cast<const DockFrequentItem*>(source)->GetItemIndex();
            if (index >= items_.size() || items_[index].layoutKey.empty()) return {};
            const auto key = ToUpperInvariant(items_[index].layoutKey);
            if (std::none_of(dockEntries_.begin(), dockEntries_.end(), [&](const auto& entry) {
                return entry.type == DockEntryType::DesktopItem && PathsEqualInsensitive(entry.reference, key);
            }))
                additions.push_back({DockEntryType::DesktopItem, key, true});
            continue;
        }
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
            if (child >= widgets_.size() || widgets_[child].type != DesktopWidgetType::Collection) return {};
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
            else return {};
        }
        else return {};
    }
    const auto action = keepSource && std::all_of(additions.begin(), additions.end(),
        [](const auto& entry) { return entry.keepOnDesktop; }) ? DropAction::Copy : DropAction::Move;
    if (additions.empty() || !(allowed & DropEffectFromAction(action))) return {};

    services.capacity = [targetDock](size_t count) { return targetDock->HasCapacity(count); };
    if (!snowdesktop::dock_drop_model::Pin({dockEntries_, widgets_}, additions, insertIndex, services))
    {
        MessageBeep(MB_ICONWARNING);
        return {};
    }
    RefreshCollectedKeysCache();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    return {action};
}

bool DesktopApp::AddMaterializedItemsToDock(
    const std::vector<std::wstring>& createdPaths,
    size_t insertIndex, bool keepOnDesktop, std::optional<DockEntrySection> section)
{
    auto services = DockDropServices();
    services.dropSection = section;
    if (!snowdesktop::dock_drop_model::AddMaterialized({dockEntries_, widgets_}, createdPaths,
            insertIndex, keepOnDesktop, services)) return false;
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

DropReceipt DesktopApp::DropItemsIntoDockCollection(const ItemDrop& drop, DockEntryItem* targetItem)
{
    if (!targetItem || !IsLogicalDockEntryType(targetItem->GetEntryType())) return {};
    const size_t index = FindWidgetIndexById(targetItem->GetReference());
    if (index >= widgets_.size()) return {};
    auto& model = widgets_[index];
    const auto& sources = drop.sources;
    const auto& sourceItems = drop.items;
    const auto point = drop.point;
    const auto mods = drop.modifiers;
    const auto allowed = drop.allowedEffects;
    auto commit = [&](WidgetContainer& target) {
        auto preview = BuildDropPreviewList(sources, &target, nullptr, HitRegion::SortAfter, mods, point);
        // A Dock group entry represents the entire group, independent of the desktop tab geometry.
        preview.groupInsertIndex = model.childWidgetIds.size();
        return target.OnItemsDropped({sourceItems, sources, preview, nullptr, HitRegion::SortAfter, mods, point, allowed});
    };
    if (IsGroupWidgetType(model.type))
    {
        if (!sources.HasDesktopWidgetIcons() && (sources.UsesFileGroupSourceInsertion() ||
            sources.SlotPayloadKind() == snowdesktop::slot_contract::DragPayloadKind::CollectionWidget ||
            sources.hasCollectionGroupEntries))
            return DropReceipt::Submitted((allowed & DROPEFFECT_MOVE) &&
                MoveWidgetSourcesToGroup(sourceItems, index, model.childWidgetIds.size()), DropAction::Move);
        for (const auto& container : containers_)
            if (auto* group = dynamic_cast<WidgetContainer*>(container.get()); group && group->GetWidgetData() == &model)
                return commit(*group);
        return {};
    }
    if (model.type == DesktopWidgetType::FileCategories)
    {
        FileCategories view(&model, this);
        return commit(view);
    }
    if (model.type != DesktopWidgetType::Collection) return {};
    Collection view(&model, this);
    return commit(view);
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

bool DesktopApp::MoveDockItemsToDesktop(
    const std::vector<Item*>& sourceItems, GridCell targetCell)
{
    const auto landings = PlanDockDesktopReturn(sourceItems, std::move(targetCell));
    // Delete-key/menu returns may animate. A drag already supplies its own
    // visual handoff and must release the source slot immediately.
    for (const auto& landing : landings)
        if (landing.accepted)
            for (const auto& container : containers_)
                if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
                    dock->PrepareEntryRemoval(landing.entryIndex);
    if (!snowdesktop::dock_desktop_return::Apply(dockEntries_, widgets_, items_, landings, DockDesktopReturnServices())) return false;
    NormalizeDockRecycleBinPosition();
    LayoutItems();
    return true;
}

void DesktopApp::RestoreDockEntriesToDesktop()
{
    const auto isSearch = [](const DockEntry& entry) { return entry.type == DockEntryType::QuickSearch; };
    if (std::all_of(dockEntries_.begin(), dockEntries_.end(), isSearch)) return;
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
        if (entry.keepOnDesktop || IsDockLayoutEntry(entry.type)) continue;
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
    std::erase_if(dockEntries_, [&](const DockEntry& entry) { return !isSearch(entry); });
    RefreshCollectedKeysCache();
}
