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
        return entry.type == DockEntryType::Collection && entry.reference == id;
    });
}

inline DockContainer* DesktopApp::GetDockContainer() const
{
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get())) return dock;
    return nullptr;
}

inline int DesktopApp::GetGridPageItemIconSize(const GridPage& page) const
{
    const int pitchX = page.cellWidth + (page.columns > 1 ? page.gapX : 0);
    const int pitchY = page.cellHeight + (page.rows > 1 ? page.gapY : 0);
    const float layoutScale = std::max(0.1f, std::min(
        static_cast<float>(std::max(1, pitchX)) / static_cast<float>(kCellWidth),
        static_cast<float>(std::max(1, pitchY)) / static_cast<float>(kMinCellHeight)));
    const int inset = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    if (page.cellHeight < static_cast<int>(std::round(50.0f * layoutScale)))
    {
        return std::max(1, std::min({
            static_cast<int>(std::round(32.0f * layoutScale)),
            std::max(1, page.cellWidth - inset * 2),
            std::max(1, page.cellHeight - inset * 2) }));
    }
    const float lineHeight = itemFontSize_ * 7.0f / 6.0f * layoutScale;
    const int textHeight = std::max(1,
        static_cast<int>(std::floor(lineHeight * 2.0f)) - 1);
    return std::max(1, std::min(
        std::max(1, page.cellWidth - inset * 2),
        std::max(1, page.cellHeight - textHeight - inset * 2)));
}

inline int DesktopApp::GetDockItemIconSize() const
{
    const GridPage* page = GetFirstPageGridPage();
    return page ? GetGridPageItemIconSize(*page) : kIconSize;
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

    const RECT originalWorkArea = it->workArea;
    const int width = std::max(1, static_cast<int>(originalWorkArea.right - originalWorkArea.left));
    const int height = std::max(1, static_cast<int>(originalWorkArea.bottom - originalWorkArea.top));
    const bool vertical = dockSettings_.position == DockPosition::Left ||
        dockSettings_.position == DockPosition::Right;
    const int edgeExtent = vertical ? width : height;
    auto reserveEdge = [&](GridPage& page, int reserved, RECT* dockArea) {
        page.workArea = originalWorkArea;
        RECT area{};
        switch (dockSettings_.position)
        {
        case DockPosition::Top:
            area = RECT{ originalWorkArea.left, originalWorkArea.top,
                originalWorkArea.right, originalWorkArea.top + reserved };
            page.workArea.top = area.bottom;
            break;
        case DockPosition::Left:
            area = RECT{ originalWorkArea.left, originalWorkArea.top,
                originalWorkArea.left + reserved, originalWorkArea.bottom };
            page.workArea.left = area.right;
            break;
        case DockPosition::Right:
            area = RECT{ originalWorkArea.right - reserved, originalWorkArea.top,
                originalWorkArea.right, originalWorkArea.bottom };
            page.workArea.right = area.left;
            break;
        case DockPosition::Bottom:
        default:
            area = RECT{ originalWorkArea.left, originalWorkArea.bottom - reserved,
                originalWorkArea.right, originalWorkArea.bottom };
            page.workArea.bottom = area.top;
            break;
        }
        if (dockArea) *dockArea = area;
    };

    // Match the Dock thickness and its two edge gaps to the recalculated 1x1
    // component thickness and the grid's component-side margin.
    GridPage bestPage = *it;
    int bestReserved = 1;
    int bestError = INT_MAX;
    for (int reserved = 1; reserved < edgeExtent; ++reserved)
    {
        GridPage candidate = *it;
        reserveEdge(candidate, reserved, nullptr);
        ApplyIconSpacingToPage(candidate);
        const int componentMargin = vertical
            ? candidate.marginX : candidate.marginY;
        const int edgeDistance = std::max(kDockSpacing, componentMargin);
        const int innerGap = edgeDistance - componentMargin;
        const int panelThickness = GetGridPageItemIconSize(candidate) +
            kDockSpacing * 2;
        const int desiredReservation = dockSettings_.edgeAttached
            ? panelThickness + innerGap
            : panelThickness + edgeDistance + innerGap;
        const int error = std::abs(desiredReservation - reserved);
        if (error < bestError)
        {
            bestError = error;
            bestReserved = reserved;
            bestPage = candidate;
            if (error == 0) break;
        }
    }

    *it = bestPage;
    reserveEdge(*it, bestReserved, &dockArea_);
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
        if (data && data->type == DesktopWidgetType::Collection)
            additions.push_back({ DockEntryType::Collection, data->id, false });
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
            if (addition.type == DockEntryType::Collection)
            {
                existing->keepOnDesktop = false;
                size_t widgetIndex = FindWidgetIndexById(addition.reference);
                if (widgetIndex < widgets_.size())
                    widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
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
        insertIndex = std::min(insertIndex, dockEntries_.size());
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex), addition);
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
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection)
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        GridCell freeCell;
        if (!FindDockReturnCell(usedSlots, targetCell.pageId, startSlot, span, freeCell))
            continue;
        MarkGridArea(usedSlots, freeCell, span);
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
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection)
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

inline void DesktopApp::DrawDockEntry(ID2D1DeviceContext* ctx,
    const DockEntry& entry, RECT rect, int state)
{
    if (!ctx) return;
    const int iconSize = GetDockItemIconSize();
    RECT iconRect{
        rect.left + (rect.right - rect.left - iconSize) / 2,
        rect.top + (rect.bottom - rect.top - iconSize) / 2,
        rect.left + (rect.right - rect.left + iconSize) / 2,
        rect.top + (rect.bottom - rect.top + iconSize) / 2
    };

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
        const float alpha = item.isCut ? 0.4f : 1.0f;
        if (item.iconState == IconState::Loading)
            DrawPlaceholderIcon(ctx, item.sysIconIndex, target, alpha);
        else if (ID2D1Bitmap1* bitmap = GetOrCreateD2DBitmap(item.iconBitmap))
            ctx->DrawBitmap(bitmap, ToD2DRect(target), alpha,
                D2D1_INTERPOLATION_MODE_LINEAR);
        else
            DrawPlaceholderIcon(ctx, item.sysIconIndex, target, alpha);
        if (ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut) &&
            item.iconState != IconState::Loading)
            DrawShortcutArrowOverlay(ctx, target, alpha);
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
    const int innerSize = std::max(1, static_cast<int>(
        std::min(iconRect.right - iconRect.left, iconRect.bottom - iconRect.top)));
    const int smallIconSize = std::max(1, (innerSize - kDockSpacing) / 2);
    const int groupSize = smallIconSize * 2 + kDockSpacing;
    const int groupLeft = iconRect.left + (innerSize - groupSize) / 2;
    const int groupTop = iconRect.top + (innerSize - groupSize) / 2;
    for (size_t i = 0; i < std::min<size_t>(4, widget.itemKeys.size()); ++i)
    {
        size_t itemIndex = FindItemIndexByKey(widget.itemKeys[i]);
        if (itemIndex >= items_.size()) continue;
        int col = static_cast<int>(i % 2);
        int row = static_cast<int>(i / 2);
        const int left = groupLeft + col * (smallIconSize + kDockSpacing);
        const int top = groupTop + row * (smallIconSize + kDockSpacing);
        RECT cell{ left, top, left + smallIconSize, top + smallIconSize };
        drawDesktopItem(items_[itemIndex], cell, 0);
    }
}
