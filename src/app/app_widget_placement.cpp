#include "app.h"

// Collision-aware widget placement and displacement planning.

void DesktopApp::PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan, bool isMove)
{
    if (widgetIndex >= widgets_.size()) return;
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return;

    targetSpan = ClampWidgetGridSpan(widgets_[widgetIndex], targetSpan,
        page->columns - targetCell.column, page->rows - targetCell.row);

    // Widget-widget collision check
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex) continue;
        if (IsGroupedWidget(widgets_[i])) continue;
        if (widgets_[i].gridCell.pageId != targetCell.pageId) continue;
        if (targetCell.column + targetSpan.columns <= widgets_[i].gridCell.column) continue;
        if (widgets_[i].gridCell.column + widgets_[i].gridSpan.columns <= targetCell.column) continue;
        if (targetCell.row + targetSpan.rows <= widgets_[i].gridCell.row) continue;
        if (widgets_[i].gridCell.row + widgets_[i].gridSpan.rows <= targetCell.row) continue;
        return; // overlaps another widget, reject
    }

    // Collect items displaced by this placement (at target location)
    std::vector<size_t> displaced;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        if (items_[i].gridCell.pageId != targetCell.pageId) continue;
        if (targetCell.column + targetSpan.columns <= items_[i].gridCell.column) continue;
        if (items_[i].gridCell.column + items_[i].gridSpan.columns <= targetCell.column) continue;
        if (targetCell.row + targetSpan.rows <= items_[i].gridCell.row) continue;
        if (items_[i].gridCell.row + items_[i].gridSpan.rows <= targetCell.row) continue;
        displaced.push_back(i);
    }

    // The widget's old cell
    GridCell oldCell = widgets_[widgetIndex].gridCell;
    GridSpan oldSpan = widgets_[widgetIndex].gridSpan;

    // Build occupied slot set (all widgets except self + non-displaced items)
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex) continue;
        if (IsGroupedWidget(widgets_[i])) continue;
        MarkGridArea(usedSlots, widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    // Mark the new target area as occupied
    MarkGridArea(usedSlots, targetCell, targetSpan);

    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        bool isDisplaced = std::find(displaced.begin(), displaced.end(), i) != displaced.end();
        if (!isDisplaced)
        {
            if (isMove)
            {
                // For move: also free items that overlap the old widget area,
                // so displaced items can be placed there.
                if (items_[i].gridCell.pageId == oldCell.pageId &&
                    !(items_[i].gridCell.column + items_[i].gridSpan.columns <= oldCell.column) &&
                    !(oldCell.column + oldSpan.columns <= items_[i].gridCell.column) &&
                    !(items_[i].gridCell.row + items_[i].gridSpan.rows <= oldCell.row) &&
                    !(oldCell.row + oldSpan.rows <= items_[i].gridCell.row))
                    continue;
            }
            MarkGridArea(usedSlots, items_[i].gridCell, items_[i].gridSpan);
        }
    }

    // 本次放置批内新建的虚拟溢出页及其下一个空闲槽位
    std::unordered_map<std::wstring, int> newPageSlots;
    // Quick-lookup of page IDs currently visible in gridPages_
    std::unordered_set<std::wstring> visiblePageIds;
    for (const auto& gp : gridPages_)
        visiblePageIds.insert(gp.id);

    // 兜底安置：已保存的离屏页 → 本批新建页（从记录的下一个槽位顺序填满）→
    // 所有现有页都满时在末屏新建虚拟溢出页，并从 slot 0 开始占满。
    auto placeOnSavedOrNewPage = [&](const GridSpan& span, GridCell& found) -> bool {
        for (const auto& pageId : savedPageIds_)
        {
            if (visiblePageIds.count(pageId)) continue;
            if (!savedPageColumns_.count(pageId) || !savedPageRows_.count(pageId)) continue;
            const int cols = savedPageColumns_[pageId];
            const int rows = savedPageRows_[pageId];
            const int capacity = std::max(1, cols * rows);
            for (int slot = 0; slot < capacity; ++slot)
            {
                GridCell candidate;
                candidate.pageId = pageId;
                candidate.column = slot / std::max(1, rows);
                candidate.row    = slot % std::max(1, rows);
                if (candidate.column + span.columns <= cols &&
                    candidate.row + span.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    found = candidate;
                    return true;
                }
            }
        }

        // 本次批内已创建的新页：从记录的下一个槽位继续顺序填满
        for (auto& [pageId, nextSlot] : newPageSlots)
        {
            const int cols = savedPageColumns_.count(pageId) ? savedPageColumns_[pageId] : 1;
            const int rows = savedPageRows_.count(pageId) ? savedPageRows_[pageId] : 1;
            const int capacity = std::max(1, cols * rows);
            for (int slot = nextSlot; slot < capacity; ++slot)
            {
                GridCell candidate;
                candidate.pageId = pageId;
                candidate.column = slot / std::max(1, rows);
                candidate.row    = slot % std::max(1, rows);
                if (candidate.column + span.columns <= cols &&
                    candidate.row + span.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    found = candidate;
                    nextSlot = slot + 1;
                    return true;
                }
            }
        }

        // 所有现有页都满 → 在末屏新建虚拟溢出页
        std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
        if (monitorOrder.empty()) return false;
        const GridPage& lastPage = gridPages_[monitorOrder.back()];

        const std::wstring newPageId = GeneratePageId();
        RememberSavedPageId(newPageId);
        savedPageColumns_[newPageId] = lastPage.columns;
        savedPageRows_[newPageId]    = lastPage.rows;

        found = { newPageId, 0, 0 };
        newPageSlots[newPageId] = 1; // slot 0 taken
        return true;
    };

    if (isMove)
    {
        // For move: displaced items go to the widget's old position area
        auto byGrid = [this](size_t a, size_t b) {
            if (items_[a].gridCell.pageId != items_[b].gridCell.pageId)
                return items_[a].gridCell.pageId < items_[b].gridCell.pageId;
            int sa = SlotFromCell(gridPages_, items_[a].gridCell);
            int sb = SlotFromCell(gridPages_, items_[b].gridCell);
            return sa < sb;
        };
        std::sort(displaced.begin(), displaced.end(), byGrid);

        widgets_[widgetIndex].gridCell = targetCell;
        widgets_[widgetIndex].gridSpan = targetSpan;

        int oldAreaSlot = SlotFromCell(gridPages_, oldCell);
        for (size_t idx : displaced)
        {
            GridCell freeCell;
            if (TryFindFreeCell(items_[idx].gridSpan, usedSlots, freeCell, oldCell.pageId, oldAreaSlot))
            {
                items_[idx].gridCell = freeCell;
                items_[idx].slot = SlotFromCell(gridPages_, freeCell);
                MarkGridArea(usedSlots, freeCell, items_[idx].gridSpan);
            }
            else if (TryFindFreeCell(items_[idx].gridSpan, usedSlots, freeCell, targetCell.pageId, 0))
            {
                items_[idx].gridCell = freeCell;
                items_[idx].slot = SlotFromCell(gridPages_, freeCell);
                MarkGridArea(usedSlots, freeCell, items_[idx].gridSpan);
            }
            else
            {
                // 无可见空位 — 已存离屏页 → 本批新建页 → 新建虚拟溢出页并占满
                GridCell landing;
                if (placeOnSavedOrNewPage(items_[idx].gridSpan, landing))
                {
                    items_[idx].gridCell = landing;
                    items_[idx].slot = SlotFromCell(gridPages_, landing);
                    MarkGridArea(usedSlots, landing, items_[idx].gridSpan);
                }
            }
        }
    }
    else
    {
        // For resize: push displaced items to new free cells
        auto byGrid = [this](size_t a, size_t b) {
            if (items_[a].gridCell.pageId != items_[b].gridCell.pageId)
                return items_[a].gridCell.pageId < items_[b].gridCell.pageId;
            int sa = SlotFromCell(gridPages_, items_[a].gridCell);
            int sb = SlotFromCell(gridPages_, items_[b].gridCell);
            return sa < sb;
        };
        std::sort(displaced.begin(), displaced.end(), byGrid);

        widgets_[widgetIndex].gridCell = targetCell;
        widgets_[widgetIndex].gridSpan = targetSpan;

        int searchStart = SlotFromCell(gridPages_, targetCell) + std::max(1, targetSpan.rows);
        for (size_t idx : displaced)
        {
            GridCell freeCell;
            if (TryFindFreeCell(items_[idx].gridSpan, usedSlots, freeCell, targetCell.pageId, searchStart))
            {
                items_[idx].gridCell = freeCell;
                items_[idx].slot = SlotFromCell(gridPages_, freeCell);
                MarkGridArea(usedSlots, freeCell, items_[idx].gridSpan);
            }
            else
            {
                // 无可见空位 — 已存离屏页 → 本批新建页 → 新建虚拟溢出页并占满
                GridCell landing;
                if (placeOnSavedOrNewPage(items_[idx].gridSpan, landing))
                {
                    items_[idx].gridCell = landing;
                    items_[idx].slot = SlotFromCell(gridPages_, landing);
                    MarkGridArea(usedSlots, landing, items_[idx].gridSpan);
                }
            }
        }
    }

    if (widgets_[widgetIndex].type == DesktopWidgetType::Collection &&
        widgets_[widgetIndex].scrollContainerMode &&
        (targetSpan.columns < 2 || targetSpan.rows < 2))
    {
        widgets_[widgetIndex].scrollContainerMode = false;
        widgets_[widgetIndex].scrollOffset = 0;
    }

    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
}
