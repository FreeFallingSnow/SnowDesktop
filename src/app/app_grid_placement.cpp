#include "app.h"

// Grid occupancy, free-cell search and displaced-item relayout.

void DesktopApp::MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
{
    for (int c = cell.column; c < cell.column + span.columns; ++c)
        for (int r = cell.row; r < cell.row + span.rows; ++r)
            usedSlots.insert(cell.pageId + L":" + std::to_wstring(c) + L"," + std::to_wstring(r));
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
    for (int c = cell.column; c < cell.column + span.columns; ++c)
        for (int r = cell.row; r < cell.row + span.rows; ++r)
            if (usedSlots.count(cell.pageId + L":" + std::to_wstring(c) + L"," + std::to_wstring(r)))
                return true;
    return false;
}

/**
 * @brief 判断网格区域是否合法（跨度 >=1，行列非负）。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 合法返回 true。
 */
bool DesktopApp::IsGridAreaValid(const GridCell& cell, GridSpan span)
{
    if (span.columns < 1 || span.rows < 1) return false;
    if (cell.column < 0 || cell.row < 0) return false;
    return true;
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
    // Automatic placement follows the configured first-page monitor order,
    // not the physical left-to-right monitor order.
    std::vector<size_t> pageOrder = BuildMonitorRenderOrder();
    if (pageOrder.empty())
    {
        pageOrder.reserve(gridPages_.size());
        for (size_t i = 0; i < gridPages_.size(); ++i)
            pageOrder.push_back(i);
    }

    auto tryPage = [&](const GridPage& page, int startSlot, GridCell& found) -> bool {
        const int capacity = std::max(1, page.columns * page.rows);
        for (int slot = std::clamp(startSlot, 0, capacity - 1); slot < capacity; ++slot)
        {
            GridCell candidate;
            candidate.pageId = page.id;
            candidate.column = slot / std::max(1, page.rows);
            candidate.row = slot % std::max(1, page.rows);
            if (GridAreaFitsPage(page, candidate, span) && !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                found = candidate;
                return true;
            }
        }
        return false;
    };

    if (!preferredPageId.empty())
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == preferredPageId && tryPage(page, preferredStartSlot, result))
                return true;
        }
    }

    for (size_t pageIndex : pageOrder)
    {
        if (pageIndex >= gridPages_.size()) continue;
        const auto& page = gridPages_[pageIndex];
        if (!preferredPageId.empty() && page.id == preferredPageId) continue;
        if (tryPage(page, 0, result))
            return true;
    }

    if (!preferredPageId.empty())
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == preferredPageId && tryPage(page, 0, result))
                return true;
        }
    }
    return false;
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
            item.gridCell = freeCell;
            item.slot = SlotFromCell(gridPages_, freeCell);
            MarkGridArea(usedSlots, freeCell, item.gridSpan);
        }
    }
}

/**
 * @brief 按名称对桌面图标排序（在每个页面内）。
 */
