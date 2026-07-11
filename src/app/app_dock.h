#pragma once

extern inline const GridPage* FindGridPage(
    const std::vector<GridPage>& pages, const std::wstring& pageId);
extern inline int SlotFromCell(
    const std::vector<GridPage>& pages, const GridCell& cell);

inline size_t DesktopApp::FindWidgetIndexById(const std::wstring& id) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].id == id) return i;
    return static_cast<size_t>(-1);
}

inline bool DesktopApp::IsDockExclusiveItemKey(const std::wstring& key) const
{
    const std::wstring upper = ToUpperInvariant(key);
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            ToUpperInvariant(entry.reference) == upper;
    });
}

inline bool DesktopApp::IsDockExclusiveWidgetId(const std::wstring& id) const
{
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return entry.type == DockEntryType::Collection && !entry.keepOnDesktop &&
            entry.reference == id;
    });
}

inline DockContainer* DesktopApp::GetDockContainer() const
{
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get())) return dock;
    return nullptr;
}

inline void DesktopApp::ApplyDockWorkAreaReservation()
{
    dockArea_ = {};
    if (!generalSettings_.dockEnabled || gridPages_.empty()) return;

    const GridPage* first = GetFirstPageGridPage();
    if (!first) return;
    auto it = std::find_if(gridPages_.begin(), gridPages_.end(),
        [&](const GridPage& page) { return &page == first; });
    if (it == gridPages_.end()) return;

    const int height = std::max(1, static_cast<int>(it->workArea.bottom - it->workArea.top));
    const int preferred = std::min(kDockReservedHeight, std::max(1, height - 1));
    const int reserved = height >= 144 ? std::max(72, preferred) : preferred;
    dockArea_ = RECT{ it->workArea.left, it->workArea.bottom - reserved,
        it->workArea.right, it->workArea.bottom };
    it->workArea.bottom = std::max<LONG>(it->workArea.top + 1, dockArea_.top);
    ApplyIconSpacingToPage(*it);
}

inline void DesktopApp::CommitDockDrop(const std::vector<Item*>& sourceItems,
    Container* origin, size_t insertIndex, int mods)
{
    DockContainer* dock = GetDockContainer();
    if (!dock || sourceItems.empty()) return;

    if (origin == dock)
    {
        std::vector<size_t> indices;
        for (Item* item : sourceItems)
            if (auto* dockItem = dynamic_cast<DockEntryItem*>(item))
                indices.push_back(dockItem->GetEntryIndex());
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.empty()) return;

        std::vector<DockEntry> moving;
        for (size_t index : indices)
            if (index < dockEntries_.size()) moving.push_back(dockEntries_[index]);
        for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        {
            if (*it < insertIndex) --insertIndex;
            dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        insertIndex = std::min(insertIndex, dockEntries_.size());
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            moving.begin(), moving.end());
        dock->InvalidateSlots();
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
        if (data && data->type == DesktopWidgetType::Collection &&
            data->gridSpan.columns == 1 && data->gridSpan.rows == 1)
            additions.push_back({ DockEntryType::Collection, data->id, keepSource });
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
    if (!dock->HasCapacity(genuinelyNew))
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
            if (becomingExclusive)
            {
                if (addition.type == DockEntryType::DesktopItem)
                {
                    RemoveDesktopKeysFromWidgets({ addition.reference });
                    size_t itemIndex = FindItemIndexByKey(addition.reference);
                    if (itemIndex < items_.size())
                        items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
                }
                else
                {
                    size_t widgetIndex = FindWidgetIndexById(addition.reference);
                    if (widgetIndex < widgets_.size())
                        widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
                }
            }
            continue;
        }
        insertIndex = std::min(insertIndex, dockEntries_.size());
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex), addition);
        ++insertIndex;

        if (addition.keepOnDesktop) continue;
        if (addition.type == DockEntryType::DesktopItem)
        {
            RemoveDesktopKeysFromWidgets({ addition.reference });
            size_t itemIndex = FindItemIndexByKey(addition.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(addition.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
        }
    }
    RefreshCollectedKeysCache();
    dock->InvalidateSlots();
    InvalidateDragStaticScene();
}

inline void DesktopApp::AddExternalItemsToDock(
    const std::vector<std::wstring>& newKeys, size_t insertIndex)
{
    DockContainer* dock = GetDockContainer();
    if (!dock || !dock->HasCapacity(newKeys.size())) return;
    for (const std::wstring& key : newKeys)
    {
        const std::wstring upper = ToUpperInvariant(key);
        if (upper.empty()) continue;
        bool exists = std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& entry) {
                return entry.type == DockEntryType::DesktopItem &&
                    ToUpperInvariant(entry.reference) == upper;
            });
        if (exists) continue;
        insertIndex = std::min(insertIndex, dockEntries_.size());
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            DockEntry{ DockEntryType::DesktopItem, upper, false });
        ++insertIndex;
        size_t itemIndex = FindItemIndexByKey(upper);
        if (itemIndex < items_.size()) items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
    }
    RefreshCollectedKeysCache();
    RebuildContainersAndItems();
    LayoutItems();
}

inline bool DesktopApp::FindDockReturnCell(
    std::unordered_set<std::wstring>& usedSlots,
    const std::wstring& preferredPageId, int startSlot, GridCell& result)
{
    if (TryFindFreeCell({ 1, 1 }, usedSlots, result, preferredPageId, startSlot))
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
            if (!AreGridSlotsMarked(usedSlots, candidate, { 1, 1 }))
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
    savedPageColumns_[pageId] = std::max(1, reference.columns);
    savedPageRows_[pageId] = std::max(1, reference.rows);
    result = { pageId, 0, 0 };
    return true;
}

inline bool DesktopApp::DropItemsIntoDockCollection(
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

inline void DesktopApp::MoveDockItemsToDesktop(
    const std::vector<Item*>& sourceItems, GridCell targetCell)
{
    std::vector<size_t> indices;
    for (Item* source : sourceItems)
        if (auto* item = dynamic_cast<DockEntryItem*>(source))
            indices.push_back(item->GetEntryIndex());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty()) return;

    std::vector<DockEntry> moving;
    for (size_t index : indices)
        if (index < dockEntries_.size()) moving.push_back(dockEntries_[index]);

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    const GridPage* targetPage = FindGridPage(gridPages_, targetCell.pageId);
    int startSlot = targetPage ? SlotFromCell(gridPages_, targetCell) : 0;
    for (const DockEntry& entry : moving)
    {
        if (entry.keepOnDesktop) continue;
        GridCell freeCell;
        if (!FindDockReturnCell(usedSlots, targetCell.pageId, startSlot, freeCell))
            continue;
        MarkGridArea(usedSlots, freeCell, { 1, 1 });
        ++startSlot;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = freeCell;
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = freeCell;
        }
    }

    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
    RefreshCollectedKeysCache();
    RebuildContainersAndItems();
    LayoutItems();
    InvalidateDragStaticScene();
}

inline void DesktopApp::RestoreDockEntriesToDesktop()
{
    if (dockEntries_.empty()) return;
    const GridPage* first = GetFirstPageGridPage();
    const std::wstring preferredPage = first ? first->id : L"";
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    int startSlot = 0;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.keepOnDesktop) continue;
        GridCell cell;
        if (!FindDockReturnCell(usedSlots, preferredPage, startSlot, cell)) continue;
        MarkGridArea(usedSlots, cell, { 1, 1 });
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

inline void DesktopApp::DrawDockEntry(ID2D1DeviceContext* ctx,
    const DockEntry& entry, RECT rect, int state)
{
    if (!ctx) return;
    RECT iconRect = rect;
    InflateRect(&iconRect, -9, -9);

    auto drawDesktopItem = [&](const DesktopItem& item, RECT target, int visualState) {
        if (visualState > 0)
        {
            DrawD2DRoundedRectangle(ctx, target, 10.0f,
                visualState == 2
                    ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.24f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
                visualState == 2
                    ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.72f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
        }
        const int width = std::max(1, static_cast<int>(target.right - target.left));
        const int height = std::max(1, static_cast<int>(target.bottom - target.top));
        const int size = std::max(1, std::min({ 56, width - 8, height - 8 }));
        RECT bitmapRect{
            target.left + (width - size) / 2,
            target.top + (height - size) / 2,
            target.left + (width + size) / 2,
            target.top + (height + size) / 2
        };
        const float alpha = item.isCut ? 0.4f : 1.0f;
        if (item.iconState == IconState::Loading)
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapRect, alpha);
        else if (ID2D1Bitmap1* bitmap = GetOrCreateD2DBitmap(item.iconBitmap))
            ctx->DrawBitmap(bitmap, ToD2DRect(bitmapRect), alpha,
                D2D1_INTERPOLATION_MODE_LINEAR);
        else
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapRect, alpha);
        if (ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut) &&
            item.iconState != IconState::Loading)
            DrawShortcutArrowOverlay(ctx, bitmapRect, alpha);
    };

    if (entry.type == DockEntryType::DesktopItem)
    {
        size_t index = FindItemIndexByKey(entry.reference);
        if (index >= items_.size()) return;
        drawDesktopItem(items_[index], iconRect, state);
        return;
    }

    size_t widgetIndex = FindWidgetIndexById(entry.reference);
    if (widgetIndex >= widgets_.size()) return;
    const DesktopWidget& widget = widgets_[widgetIndex];
    RECT inner = iconRect;
    InflateRect(&inner, -1, -1);
    const int halfW = std::max(1L, (inner.right - inner.left) / 2);
    const int halfH = std::max(1L, (inner.bottom - inner.top) / 2);
    for (size_t i = 0; i < std::min<size_t>(4, widget.itemKeys.size()); ++i)
    {
        size_t itemIndex = FindItemIndexByKey(widget.itemKeys[i]);
        if (itemIndex >= items_.size()) continue;
        int col = static_cast<int>(i % 2);
        int row = static_cast<int>(i / 2);
        RECT cell{ inner.left + col * halfW, inner.top + row * halfH,
            inner.left + (col + 1) * halfW, inner.top + (row + 1) * halfH };
        InflateRect(&cell, -1, -1);
        drawDesktopItem(items_[itemIndex], cell, 0);
    }
}
