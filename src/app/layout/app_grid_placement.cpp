#include "app/app.h"
#include "layout/grid_occupancy.h"

// Grid occupancy, free-cell search and displaced-item relayout.

void DesktopApp::MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
{
    snowdesktop::grid_occupancy::Mark(usedSlots, cell, span);
}

/**
 * @brief 检查某个网格区域是否有任何格子已被标记。
 * @param usedSlots 已占用格子集合。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 如果有任何格子被标记返回 true。
 */
bool DesktopApp::AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
{
    return snowdesktop::grid_occupancy::Occupied(usedSlots, cell, span);
}

/**
 * @brief 判断网格区域是否合法（跨度 >=1，行列非负）。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 合法返回 true。
 */
bool DesktopApp::IsGridAreaValid(const GridCell& cell, GridSpan span)
{
    return snowdesktop::grid_occupancy::Valid(cell, span);
}

/**
 * @brief 尝试在网格中查找一个空闲单元格以放置指定跨度的项目。
 * @param span 所需跨度。
 * @param usedSlots 已占用的格子集合。
 * @param result 输出参数，找到的空闲单元格。
 * @param preferredPageId 首选页面 ID。
 * @param preferredStartSlot 首选起始槽位。
 * @return 找到返回 true。
 */
bool DesktopApp::TryFindFreeCell(
    GridSpan span, std::unordered_set<std::wstring>& usedSlots, GridCell& result,
    const std::wstring& preferredPageId, int preferredStartSlot) const
{
    return snowdesktop::grid_occupancy::FindFree(gridPages_, BuildMonitorRenderOrder(),
        span, usedSlots, result, preferredPageId, preferredStartSlot);
}
/**
 * @brief 重新放置所有因页面尺寸变化而被移出边界的项目和组件。
 *
 * 对于无法放入原位置的项目，自动扩展页面或寻找空闲单元格安置。
 */
void DesktopApp::RelayoutDisplacedItems()
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    std::unordered_set<std::wstring> usedSlots;
    // Track newly created virtual pages and their next free slot index
    std::unordered_map<std::wstring, int> newPageSlots;

    auto tryPlaceOnPage = [&](const std::wstring& pageId, int columns, int rows,
                               int& nextSlot, GridSpan span, GridCell& found) -> bool {
        const int capacity = std::max(1, columns * rows);
        for (int slot = nextSlot; slot < capacity; ++slot)
        {
            GridCell candidate;
            candidate.pageId = pageId;
            candidate.column = slot / std::max(1, rows);
            candidate.row    = slot % std::max(1, rows);
            if (candidate.column + span.columns <= columns &&
                candidate.row + span.rows <= rows &&
                !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                found = candidate;
                nextSlot = slot + 1;
                return true;
            }
        }
        return false;
    };

    // Build a quick-lookup set of page IDs currently visible in gridPages_
    std::unordered_set<std::wstring> visiblePageIds;
    for (const auto& gp : gridPages_)
        visiblePageIds.insert(gp.id);

    auto findFreeCellOrGrow = [&](GridSpan span, GridCell& result, const std::wstring& preferredPageId) -> bool {
        if (TryFindFreeCell(span, usedSlots, result, preferredPageId))
            return true;

        // Search all saved pages that aren't currently visible (virtual pages at other offsets)
        for (const auto& pageId : savedPageIds_)
        {
            if (visiblePageIds.count(pageId)) continue;   // already tried via TryFindFreeCell
            if (!savedPageColumns_.count(pageId) || !savedPageRows_.count(pageId)) continue;
            int cols = savedPageColumns_[pageId];
            int rows = savedPageRows_[pageId];
            int dummySlot = 0;
            if (tryPlaceOnPage(pageId, cols, rows, dummySlot, span, result))
                return true;
        }

        // Try previously-created new pages in this batch before creating another
        for (auto& [pageId, nextSlot] : newPageSlots)
        {
            int cols = savedPageColumns_.count(pageId) ? savedPageColumns_[pageId] : 1;
            int rows = savedPageRows_.count(pageId) ? savedPageRows_[pageId] : 1;
            if (tryPlaceOnPage(pageId, cols, rows, nextSlot, span, result))
                return true;
        }

        // No space anywhere — create a new virtual page on the last monitor
        std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
        if (monitorOrder.empty()) return false;

        GridPage& lastPage = gridPages_[monitorOrder.back()];

        std::wstring newPageId = GeneratePageId();
        RememberSavedPageId(newPageId);
        savedPageColumns_[newPageId] = lastPage.columns;
        savedPageRows_[newPageId]    = lastPage.rows;

        result.pageId = newPageId;
        result.column = 0;
        result.row    = 0;
        newPageSlots[newPageId] = 1; // slot 0 taken
        return true;
    };

    std::vector<size_t> displacedWidgets;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        auto& widget = widgets_[i];
        const bool dockExclusive =
            IsDockExclusiveWidgetId(widget.id);
        if (!snowdesktop::item_layout_rules::
                ShouldRelayoutDesktopWidget(
                    IsGroupedWidget(widget),
                    dockExclusive))
        {
            if (dockExclusive)
                widget.gridCell = {
                    kDockPageId, 0, 0
                };
            continue;
        }
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (!page)
        {
            // Widget is on a saved page not in current gridPages_ (different pageOffset).
            // If its position is still valid per saved dimensions, keep it in place.
            const std::wstring& pid = widget.gridCell.pageId;
            if (!pid.empty() && savedPageColumns_.count(pid) && savedPageRows_.count(pid))
            {
                int cols = savedPageColumns_[pid];
                int rows = savedPageRows_[pid];
                widget.gridSpan = ClampWidgetGridSpan(widget, widget.gridSpan, cols, rows);
                if (widget.gridCell.column >= 0 && widget.gridCell.row >= 0 &&
                    widget.gridCell.column + widget.gridSpan.columns <= cols &&
                    widget.gridCell.row + widget.gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, widget.gridCell, widget.gridSpan))
                {
                    MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
                    continue;
                }
            }
            displacedWidgets.push_back(i);
            continue;
        }

        widget.gridSpan = ClampWidgetGridSpan(widget, widget.gridSpan,
            page->columns, page->rows);
        if (GridAreaFitsPage(*page, widget.gridCell, widget.gridSpan) &&
            !AreGridSlotsMarked(usedSlots, widget.gridCell, widget.gridSpan))
        {
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
        }
        else
        {
            displacedWidgets.push_back(i);
        }
    }

    for (size_t widgetIndex : displacedWidgets)
    {
        auto& widget = widgets_[widgetIndex];
        GridCell freeCell;
        if (findFreeCellOrGrow(widget.gridSpan, freeCell, widget.gridCell.pageId))
        {
            widget.gridCell = freeCell;
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
        }
    }

    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        const GridPage* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (page)
        {
            item.gridSpan.columns = std::clamp(item.gridSpan.columns, 1, std::max(1, page->columns));
            item.gridSpan.rows = std::clamp(item.gridSpan.rows, 1, std::max(1, page->rows));
            if (GridAreaFitsPage(*page, item.gridCell, item.gridSpan) &&
                !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan))
            {
                MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                continue;
            }
        }
        else if (!item.gridCell.pageId.empty())
        {
            // Item is on a saved page not in current gridPages_ (different pageOffset).
            // Mark its slot so searching those pages won't see it as free.
            const std::wstring& pid = item.gridCell.pageId;
            if (savedPageColumns_.count(pid) && savedPageRows_.count(pid))
            {
                int cols = savedPageColumns_[pid];
                int rows = savedPageRows_[pid];
                if (item.gridCell.column >= 0 && item.gridCell.row >= 0 &&
                    item.gridCell.column + item.gridSpan.columns <= cols &&
                    item.gridCell.row + item.gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan))
                {
                    MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                    continue;
                }
            }
        }

        GridCell freeCell;
        if (findFreeCellOrGrow(item.gridSpan, freeCell, item.gridCell.pageId))
        {
            if (item.largeIcon)
            {
                if (const auto* target = FindGridPage(gridPages_, freeCell.pageId))
                { item.gridSpan.columns = std::min(item.gridSpan.columns, target->columns); item.gridSpan.rows = std::min(item.gridSpan.rows, target->rows); }
                else if (savedPageColumns_.contains(freeCell.pageId) && savedPageRows_.contains(freeCell.pageId))
                { item.gridSpan.columns = std::min(item.gridSpan.columns, savedPageColumns_[freeCell.pageId]); item.gridSpan.rows = std::min(item.gridSpan.rows, savedPageRows_[freeCell.pageId]); }
            }
            item.gridCell = freeCell;
            item.slot = SlotFromCell(gridPages_, freeCell);
            MarkGridArea(usedSlots, freeCell, item.gridSpan);
        }
    }
}

/**
 * @brief 把解除收纳的桌面项按新文件规则重新寻找桌面空位。
 *
 * 被收纳期间项目保留着收纳前的网格键，期间新文件、组件或其他布局调整可能
 * 已经占用该位置。解除收纳时不能直接沿用旧位置，而应在当前占用之外重新落位；
 * 现有页面没有空位时沿用新文件的溢出页流程。
 * @param keys 解除收纳的布局键（大小写不敏感）
 * @return 至少处理了一项并需要重新布局时返回 true
 */
bool DesktopApp::PlaceReleasedDesktopItems(
    const std::vector<std::wstring>& keys)
{
    if (keys.empty() || gridPages_.empty()) return false;

    std::unordered_set<std::wstring> released;
    released.reserve(keys.size());
    for (const auto& key : keys)
        if (!key.empty()) released.insert(ToUpperInvariant(key));
    if (released.empty()) return false;

    RefreshCollectedKeysCache();
    std::unordered_set<std::wstring> usedSlots;
    std::unordered_map<std::wstring, int> newPageSlots;
    std::vector<DesktopItem*> displaced;
    displaced.reserve(released.size());
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (released.contains(ToUpperInvariant(item.layoutKey)))
        {
            if (item.gridCell.pageId != kDockPageId)
                displaced.push_back(&item);
            continue;
        }
        if (IsItemInAnyWidget(item) || item.gridCell.pageId == kDockPageId)
            continue;
        if (const GridPage* page = FindGridPage(gridPages_, item.gridCell.pageId))
        {
            if (GridAreaFitsPage(*page, item.gridCell, item.gridSpan))
                MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
            continue;
        }
        // Items on saved pages outside the current offset still own their cells.
        const std::wstring& pageId = item.gridCell.pageId;
        if (pageId.empty() || pageId == kDockPageId ||
            !savedPageColumns_.contains(pageId) ||
            !savedPageRows_.contains(pageId) ||
            item.gridCell.column < 0 || item.gridCell.row < 0 ||
            item.gridCell.column + item.gridSpan.columns >
                savedPageColumns_.at(pageId) ||
            item.gridCell.row + item.gridSpan.rows >
                savedPageRows_.at(pageId))
            continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }
    if (displaced.empty()) return false;

    const auto tryPlaceOnPage = [&](const std::wstring& pageId,
                                     int columns, int rows, int& nextSlot,
                                     GridSpan span, GridCell& found) -> bool {
        const int capacity = std::max(1, columns * rows);
        for (int slot = nextSlot; slot < capacity; ++slot)
        {
            GridCell candidate;
            candidate.pageId = pageId;
            candidate.column = slot / std::max(1, rows);
            candidate.row = slot % std::max(1, rows);
            if (candidate.column + span.columns <= columns &&
                candidate.row + span.rows <= rows &&
                !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                found = candidate;
                nextSlot = slot + 1;
                return true;
            }
        }
        return false;
    };

    std::unordered_set<std::wstring> visiblePageIds;
    visiblePageIds.reserve(gridPages_.size());
    for (const auto& page : gridPages_)
        visiblePageIds.insert(page.id);

    const auto findFreeCellOrGrow = [&](GridSpan span, GridCell& result) -> bool {
        if (TryFindFreeCell(span, usedSlots, result))
            return true;

        // Saved pages outside the current offset keep their previous placement.
        for (const auto& pageId : savedPageIds_)
        {
            if (visiblePageIds.contains(pageId)) continue;
            if (!savedPageColumns_.contains(pageId) ||
                !savedPageRows_.contains(pageId))
                continue;
            int slot = 0;
            if (tryPlaceOnPage(pageId, savedPageColumns_.at(pageId),
                    savedPageRows_.at(pageId), slot, span, result))
                return true;
        }

        // Reuse the overflow pages created earlier in this batch before adding
        // another one.
        for (auto& [pageId, nextSlot] : newPageSlots)
        {
            const auto columns = savedPageColumns_.find(pageId);
            const auto rows = savedPageRows_.find(pageId);
            if (columns == savedPageColumns_.end() ||
                rows == savedPageRows_.end())
                continue;
            if (tryPlaceOnPage(pageId, columns->second, rows->second,
                    nextSlot, span, result))
                return true;
        }

        std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
        if (monitorOrder.empty() || monitorOrder.back() >= gridPages_.size())
            return false;
        const GridPage& lastPage = gridPages_[monitorOrder.back()];
        if (span.columns < 1 || span.rows < 1 ||
            span.columns > lastPage.columns || span.rows > lastPage.rows)
            return false;

        std::wstring newPageId = GeneratePageId();
        if (newPageId.empty()) return false;
        RememberSavedPageId(newPageId);
        savedPageColumns_[newPageId] = lastPage.columns;
        savedPageRows_[newPageId] = lastPage.rows;
        result = {newPageId, 0, 0};
        newPageSlots[newPageId] = 1;
        return true;
    };

    bool changed = false;
    for (DesktopItem* item : displaced)
    {
        GridSpan span = item->gridSpan;
        if (item->largeIcon)
            span = {std::max(1, item->largeIcon->columns),
                std::max(1, item->largeIcon->rows)};
        span.columns = std::max(1, span.columns);
        span.rows = std::max(1, span.rows);
        // A large icon larger than the target page cannot be assigned anywhere.
        if (const GridPage* firstPage = GetFirstPageGridPage())
        {
            span.columns = std::min(span.columns, std::max(1, firstPage->columns));
            span.rows = std::min(span.rows, std::max(1, firstPage->rows));
        }

        GridCell landing;
        if (!findFreeCellOrGrow(span, landing))
            continue;

        changed = true;
        item->gridCell = landing;
        item->gridSpan = span;
        item->slot = SlotFromCell(gridPages_, landing);
        MarkGridArea(usedSlots, landing, span);
    }
    return changed;
}

/**
 * @brief 按名称对桌面图标排序（在每个页面内）。
 */
