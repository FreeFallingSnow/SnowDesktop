#include "app.h"
#include "../widgets/collection_group_rules.h"

// Dock insertion, collection drops and restoration to the desktop.

void DesktopApp::CommitDockDrop(const std::vector<Item*>& sourceItems,
    Container* origin, DockContainer* targetDock, size_t insertIndex, int mods)
{
    if (!targetDock || sourceItems.empty()) return;

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
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.empty()) return;

        std::vector<DockEntry> moving;
        for (size_t index : indices)
            if (index < dockEntries_.size()) moving.push_back(dockEntries_[index]);
        const bool movingFolders = std::all_of(
            moving.begin(), moving.end(),
            [this](const DockEntry& entry) {
                return IsFolderDockEntry(entry);
            });
        const bool movingMain = std::all_of(
            moving.begin(), moving.end(),
            [this](const DockEntry& entry) {
                return !IsFolderDockEntry(entry);
            });
        if (!movingFolders && !movingMain)
        {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        {
            if (*it < insertIndex) --insertIndex;
            dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        const size_t mainEnd = DockMainEntryCount();
        const size_t folderEnd =
            mainEnd + DockFolderEntryCount();
        insertIndex = movingFolders
            ? std::clamp(insertIndex, mainEnd, folderEnd)
            : std::min(insertIndex, mainEnd);
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            moving.begin(), moving.end());
        NormalizeDockRecycleBinPosition();
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
        if (data && data->type == DesktopWidgetType::Collection)
            additions.push_back({ DockEntryType::Collection, data->id, false });
        else if (data && data->type == DesktopWidgetType::FolderMapping)
            additions.push_back({ DockEntryType::FolderMapping, data->id, false });
        else if (auto* groupEntry =
                     dynamic_cast<FileGroupEntryItem*>(source))
        {
            const size_t widgetIndex =
                FindWidgetIndexById(
                    groupEntry->GetChildWidgetId());
            if (widgetIndex < widgets_.size() &&
                widgets_[widgetIndex].type ==
                    DesktopWidgetType::FolderMapping)
            {
                additions.push_back({
                    DockEntryType::FolderMapping,
                    widgets_[widgetIndex].id,
                    false });
            }
        }
    }
    if (additions.empty()) return;

    size_t genuinelyNew = 0;
    for (const DockEntry& addition : additions)
    {
        bool exists = std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& current) {
                return current.type == addition.type &&
                    ToUpperInvariant(current.reference) == ToUpperInvariant(addition.reference);
            });
        if (!exists) ++genuinelyNew;
    }
    if (!targetDock->HasCapacity(genuinelyNew))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    for (const DockEntry& addition : additions)
    {
        auto existing = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& current) {
                return current.type == addition.type &&
                    ToUpperInvariant(current.reference) == ToUpperInvariant(addition.reference);
            });
        if (existing != dockEntries_.end())
        {
            const bool becomingExclusive = existing->keepOnDesktop && !addition.keepOnDesktop;
            existing->keepOnDesktop = addition.keepOnDesktop;
            if (addition.type == DockEntryType::Collection ||
                addition.type == DockEntryType::FolderMapping)
            {
                existing->keepOnDesktop = false;
                size_t widgetIndex = FindWidgetIndexById(addition.reference);
                if (widgetIndex < widgets_.size())
                {
                    if (addition.type == DockEntryType::FolderMapping)
                    {
                        for (auto& group : widgets_)
                        {
                            if (group.type != DesktopWidgetType::FileGroup) continue;
                            std::erase(group.childWidgetIds, addition.reference);
                            group.activeCategoryId =
                                snowdesktop::collection_group_rules::
                                    ResolveActiveItem(
                                        group.childWidgetIds,
                                        group.activeCategoryId);
                        }
                    }
                    widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
                }
                continue;
            }
            if (becomingExclusive)
            {
                RemoveDesktopKeysFromWidgets({ addition.reference });
                size_t itemIndex = FindItemIndexByKey(addition.reference);
                if (itemIndex < items_.size())
                    items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
            }
            continue;
        }
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        const size_t sortableEnd = static_cast<size_t>(
            std::distance(dockEntries_.begin(), recycleBin));
        insertIndex = IsRecycleBinDockEntry(addition)
            ? dockEntries_.size()
            : std::min(insertIndex, sortableEnd);
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex), addition);
        if (!IsRecycleBinDockEntry(addition))
            ++insertIndex;

        if (addition.keepOnDesktop && addition.type == DockEntryType::DesktopItem) continue;
        if (addition.type == DockEntryType::DesktopItem)
        {
            RemoveDesktopKeysFromWidgets({ addition.reference });
            size_t itemIndex = FindItemIndexByKey(addition.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(addition.reference);
            if (widgetIndex < widgets_.size())
            {
                if (addition.type == DockEntryType::FolderMapping)
                {
                    for (auto& group : widgets_)
                    {
                        if (group.type != DesktopWidgetType::FileGroup) continue;
                        std::erase(group.childWidgetIds, addition.reference);
                        group.activeCategoryId =
                            snowdesktop::collection_group_rules::
                                ResolveActiveItem(
                                    group.childWidgetIds,
                                    group.activeCategoryId);
                    }
                }
                widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
            }
        }
    }
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
}

bool DesktopApp::AddMaterializedItemsToDock(
    const std::vector<std::wstring>& createdPaths,
    size_t insertIndex)
{
    bool hasDock = false;
    for (const auto& container : containers_)
    {
        auto* dock =
            dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        hasDock = true;
        if (!dock->HasCapacity(createdPaths.size()))
            return false;
    }
    if (!hasDock) return false;

    bool changed = false;
    for (const std::wstring& path : createdPaths)
    {
        const std::wstring upper = ToUpperInvariant(path);
        if (upper.empty() ||
            snowdesktop::
                shell_item_visibility::
                    IsAlwaysHidden(upper))
            continue;
        bool exists = std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& entry) {
                return entry.type == DockEntryType::DesktopItem &&
                    ToUpperInvariant(entry.reference) == upper;
        });
        if (exists) continue;
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        insertIndex = std::min(insertIndex,
            static_cast<size_t>(std::distance(dockEntries_.begin(), recycleBin)));
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            DockEntry{ DockEntryType::DesktopItem, upper, false });
        ++insertIndex;
        changed = true;
    }
    if (!changed) return false;
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    return true;
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
    if (!targetItem || targetItem->GetEntryType() != DockEntryType::Collection)
        return false;
    size_t widgetIndex = FindWidgetIndexById(targetItem->GetReference());
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        return false;

    Collection collection(&widgets_[widgetIndex], this);
    DragSourceList sourceList = BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = BuildDropPreviewList(sourceList, &collection,
        nullptr, HitRegion::SortAfter, mods, dragSession_.CurrentPoint());
    return ExecuteDropPipeline(sourceList, preview);
}

void DesktopApp::MoveDockItemsToDesktop(
    const std::vector<Item*>& sourceItems, GridCell targetCell)
{
    std::vector<size_t> indices;
    for (Item* source : sourceItems)
        if (auto* item = dynamic_cast<DockEntryItem*>(source))
            indices.push_back(item->GetEntryIndex());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty()) return;

    std::vector<std::pair<size_t, DockEntry>> moving;
    for (size_t index : indices)
    {
        if (index < dockEntries_.size())
            moving.emplace_back(index, dockEntries_[index]);
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (!IsGroupedWidget(widget) &&
            widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    const GridPage* targetPage = FindGridPage(gridPages_, targetCell.pageId);
    int startSlot = targetPage ? SlotFromCell(gridPages_, targetCell) : 0;
    std::vector<size_t> restoredIndices;
    for (size_t movingIndex = 0;
        movingIndex < moving.size();
        ++movingIndex)
    {
        const size_t entryIndex =
            moving[movingIndex].first;
        const DockEntry& entry =
            moving[movingIndex].second;
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection ||
            entry.type == DockEntryType::FolderMapping)
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        // 组件定位不再在被占用时寻找其他可选位置：仅当命中格的跨距
        // 完全空闲且不越界时才放置，否则拒绝放置，不自动寻找替代落点。
        GridCell freeCell;
        freeCell.pageId = targetCell.pageId;
        if (!targetPage)
        {
            freeCell.column = 0;
            freeCell.row = 0;
        }
        else
        {
            freeCell.column = startSlot / std::max(1, targetPage->rows);
            freeCell.row = startSlot % std::max(1, targetPage->rows);
            freeCell = ClampGridCellToFitPage(
                *targetPage, freeCell, span);
        }
        if (!targetPage ||
            !GridAreaFitsPage(*targetPage, freeCell, span) ||
            AreGridSlotsMarked(usedSlots, freeCell, span))
            continue;
        MarkGridArea(usedSlots, freeCell, span);
        ++startSlot;
        bool restored = false;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size())
            {
                items_[itemIndex].gridCell = freeCell;
                restored = true;
            }
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size())
            {
                widgets_[widgetIndex].gridCell = freeCell;
                restored = true;
            }
        }
        if (restored)
            restoredIndices.push_back(entryIndex);
    }

    for (auto it = restoredIndices.rbegin();
        it != restoredIndices.rend(); ++it)
        dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
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
        if (entry.type == DockEntryType::Collection ||
            entry.type == DockEntryType::FolderMapping)
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
