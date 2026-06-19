/**
 * @file app_grid.h
 * @brief DesktopApp 的网格辅助、布局持久化、拖拽、控件窗口、位图缓存及数据加载等内联实现。
 *
 * 该文件在 app_oo.h 中类定义之后被包含，提供所有网格布局计算、布局文件的读写、
 * Shell 变更通知注册、拖拽操作、控件窗口消息处理、图标位图缓存及桌面项加载等功能。
 */
#pragma once

#include "drop_model.h"

// ── 网格辅助函数 ──────────────────────────────────────────

/**
 * @brief 判断指定的网格区域是否在页面范围内。
 * @param page  目标网格页面。
 * @param cell  起始单元格。
 * @param span  跨度（列数 x 行数）。
 * @return 如果区域完全在页面边界内返回 true，否则返回 false。
 */
inline bool GridAreaFitsPage(const GridPage& page, const GridCell& cell, GridSpan span)
{
    if (span.columns < 1 || span.rows < 1) return false;
    if (cell.pageId != page.id) return false;
    if (cell.column < 0 || cell.row < 0) return false;
    return cell.column + span.columns <= page.columns &&
        cell.row + span.rows <= page.rows;
}

/**
 * @brief 根据屏幕坐标查找所在的网格页面。
 * @param point 客户区坐标。
 * @return 指向对应 GridPage 的指针，未找到时返回第一个页面或 nullptr。
 */
inline const GridPage* DesktopApp::GridPageFromPoint(POINT point) const
{
    const GridPage* fallback = gridPages_.empty() ? nullptr : &gridPages_.front();
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, point) || PtInRect(&page.workArea, point))
            return &page;
    }
    return fallback;
}

/**
 * @brief 在右键菜单所在页面调整行数（增/减）。
 * @param delta 行数变化量（正数增加，负数减少）。
 */
inline void DesktopApp::AdjustGridRows(int delta)
{
    if (gridPages_.empty()) return;
    POINT clientPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* found = GridPageFromPoint(clientPoint);
    if (!found) return;
    GridPage* targetPage = nullptr;
    for (auto& page : gridPages_)
        if (page.id == found->id) { targetPage = &page; break; }
    if (!targetPage) return;

    constexpr int kMinRows = 1;
    constexpr int kMaxRows = 50;
    const int newRows = std::clamp(targetPage->rows + delta, kMinRows, kMaxRows);
    if (newRows == targetPage->rows) return;

    targetPage->rows = newRows;
    ApplyGapScaleToPage(*targetPage);
    savedPageColumns_[targetPage->id] = targetPage->columns;
    savedPageRows_[targetPage->id] = targetPage->rows;
    RelayoutDisplacedItems();
    SaveLayoutSlots();
    LayoutItems();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 在右键菜单所在页面调整列数（增/减）。
 * @param delta 列数变化量（正数增加，负数减少）。
 */
inline void DesktopApp::AdjustGridColumns(int delta)
{
    if (gridPages_.empty()) return;
    POINT clientPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* found = GridPageFromPoint(clientPoint);
    if (!found) return;
    GridPage* targetPage = nullptr;
    for (auto& page : gridPages_)
        if (page.id == found->id) { targetPage = &page; break; }
    if (!targetPage) return;

    constexpr int kMinColumns = 1;
    constexpr int kMaxColumns = 50;
    const int newColumns = std::clamp(targetPage->columns + delta, kMinColumns, kMaxColumns);
    if (newColumns == targetPage->columns) return;

    targetPage->columns = newColumns;
    ApplyGapScaleToPage(*targetPage);
    savedPageColumns_[targetPage->id] = targetPage->columns;
    savedPageRows_[targetPage->id] = targetPage->rows;
    RelayoutDisplacedItems();
    SaveLayoutSlots();
    LayoutItems();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 将指定物理坐标的页面设为首个监控器页面。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::SetFirstPageMonitorFromPoint(POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* page = GridPageFromPoint(clientPoint);
    if (!page || page->monitorId.empty()) return;

    firstPageMonitorId_ = page->monitorId;
    pageOffset_ = 0;
    ApplyPageMapping();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 设置网格缩放比例（0.5 ~ 2.0），并重新布局。
 * @param value 新的缩放值。
 */
inline void DesktopApp::SetZoom(float value)
{
    float clamped = std::clamp(value, 0.5f, 2.0f);
    if (clamped == gapScale_) return;
    gapScale_ = clamped;
    for (auto& page : gridPages_)
        ApplyGapScaleToPage(page);
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 以增量方式调整网格缩放比例。
 * @param delta 缩放变化量。
 */
inline void DesktopApp::AdjustZoom(float delta)
{
    float newVal = std::clamp(gapScale_ + delta, 0.5f, 2.0f);
    if (newVal == gapScale_) return;
    gapScale_ = newVal;
    for (auto& page : gridPages_)
        ApplyGapScaleToPage(page);
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 获取首个监控器页面在页面列表中的索引。
 * @return 页面索引，默认返回 0。
 */
inline size_t DesktopApp::FirstMonitorOrderIndex() const
{
    if (gridPages_.empty()) return 0;
    for (size_t i = 0; i < gridPages_.size(); ++i)
        if (!firstPageMonitorId_.empty() && gridPages_[i].monitorId == firstPageMonitorId_)
            return i;
    for (size_t i = 0; i < gridPages_.size(); ++i)
        if (!primaryMonitorId_.empty() && gridPages_[i].monitorId == primaryMonitorId_)
            return i;
    return 0;
}

/**
 * @brief 构建监控器渲染顺序（以首个显示器页面为起点）。
 * @return 页面索引的顺序列表。
 */
inline std::vector<size_t> DesktopApp::BuildMonitorRenderOrder() const
{
    std::vector<size_t> order;
    if (gridPages_.empty()) return order;
    order.reserve(gridPages_.size());
    const size_t first = FirstMonitorOrderIndex();
    for (size_t offset = 0; offset < gridPages_.size(); ++offset)
        order.push_back((first + offset) % gridPages_.size());
    return order;
}

/**
 * @brief 检查指定页面是否包含任何内容（项目或组件）。
 * @param pageId 页面 ID。
 * @return 有内容返回 true，否则 false。
 */
inline bool DesktopApp::PageHasContent(const std::wstring& pageId) const
{
    if (pageId.empty()) return false;
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId == pageId) return true;
    for (const auto& w : widgets_)
        if (w.gridCell.pageId == pageId) return true;
    return false;
}

/**
 * @brief 从当前偏移位置沿指定方向查找下一个非空页面的偏移量。
 * @param fromOffset 起始偏移量。
 * @param direction 方向（1 向前 / -1 向后）。
 * @return 找到的偏移量，未找到则返回原偏移量。
 */
inline int DesktopApp::NextNonEmptyOffset(int fromOffset, int direction) const
{
    if (savedPageIds_.empty() || gridPages_.empty()) return fromOffset;
    const int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
    int offset = fromOffset;
    while (true)
    {
        offset += direction;
        if (offset < 0 || offset > static_cast<int>(savedPageIds_.size()) - visiblePageCount)
            return fromOffset;
        size_t pageIdx = static_cast<size_t>((visiblePageCount - 1) + offset);
        if (pageIdx < savedPageIds_.size() && PageHasContent(savedPageIds_[pageIdx]))
            return offset;
    }
}

/**
 * @brief 计算最大页面偏移量（最后一个有内容的页面位置）。
 * @return 最大偏移值。
 */
inline int DesktopApp::MaxPageOffset() const
{
    if (savedPageIds_.empty() || gridPages_.empty()) return 0;
    const int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
    const int rawMax = std::max(0, static_cast<int>(savedPageIds_.size()) - visiblePageCount);
    int result = 0;
    for (int off = 1; off <= rawMax; ++off)
    {
        size_t pageIdx = static_cast<size_t>((visiblePageCount - 1) + off);
        if (pageIdx < savedPageIds_.size() && PageHasContent(savedPageIds_[pageIdx]))
            result = off;
    }
    return result;
}

/**
 * @brief 将已保存的页面 ID 映射到当前网格页面，并应用保存的列/行数。
 */
inline void DesktopApp::ApplyPageMapping()
{
    lastMonitorPageId_.clear();
    if (gridPages_.empty()) return;

    if (savedPageIds_.empty())
    {
        for (const auto& page : gridPages_)
            RememberSavedPageId(page.monitorId);
    }

    pageOffset_ = std::clamp(pageOffset_, 0, MaxPageOffset());
    std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
    const size_t numMonitors = monitorOrder.size();
    for (size_t i = 0; i < numMonitors; ++i)
    {
        GridPage& page = gridPages_[monitorOrder[i]];
        const bool isLast = (i == numMonitors - 1);
        const size_t pageIdx = i + (isLast ? static_cast<size_t>(pageOffset_) : 0);
        if (pageIdx < savedPageIds_.size())
            page.id = savedPageIds_[pageIdx];
        else
            page.id = L"__extra:" + page.monitorId;
        if (isLast)
            lastMonitorPageId_ = page.id;
    }
    if (!lastMonitorPageId_.empty() && savedPageIds_.size() <= gridPages_.size())
        lastMonitorPageId_.clear();

    ApplySavedGridDimensions();
}

/**
 * @brief 在 usedSlots 集合中标记一个网格区域的所有格子被占用。
 * @param usedSlots 已占用格子集合。
 * @param cell 起始单元格。
 * @param span 跨度。
 */
inline void DesktopApp::MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
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
inline bool DesktopApp::AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
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
inline bool DesktopApp::IsGridAreaValid(const GridCell& cell, GridSpan span)
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
inline bool DesktopApp::TryFindFreeCell(
    GridSpan span, std::unordered_set<std::wstring>& usedSlots, GridCell& result,
    const std::wstring& preferredPageId, int preferredStartSlot) const
{
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

    for (const auto& page : gridPages_)
    {
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
inline void DesktopApp::RelayoutDisplacedItems()
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    std::unordered_set<std::wstring> usedSlots;
    auto findFreeCellOrGrow = [&](GridSpan span, GridCell& result, const std::wstring& preferredPageId) -> bool {
        if (TryFindFreeCell(span, usedSlots, result, preferredPageId))
            return true;

        GridPage* targetPage = nullptr;
        for (auto& page : gridPages_)
        {
            if (page.id == preferredPageId)
            {
                targetPage = &page;
                break;
            }
        }
        if (!targetPage && !gridPages_.empty())
            targetPage = &gridPages_.front();
        if (!targetPage)
            return false;

        constexpr int kMaxRows = 50;
        while (targetPage->rows < kMaxRows)
        {
            ++targetPage->rows;
            ApplyGapScaleToPage(*targetPage);
            savedPageColumns_[targetPage->id] = targetPage->columns;
            savedPageRows_[targetPage->id] = targetPage->rows;
            if (TryFindFreeCell(span, usedSlots, result, targetPage->id))
                return true;
        }
        return false;
    };

    std::vector<size_t> displacedWidgets;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        auto& widget = widgets_[i];
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (!page)
        {
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
inline void DesktopApp::SortIconsByName()
{
    auto sortForPage = [&](const GridPage& page) {
        std::vector<size_t> order;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].name.empty() || IsItemInAnyWidget(items_[i])) continue;
            if (items_[i].gridCell.pageId.empty())
                items_[i].gridCell.pageId = gridPages_.empty() ? L"" : gridPages_.front().id;
            if (items_[i].gridCell.pageId == page.id)
                order.push_back(i);
        }

        std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
            return ToUpperInvariant(items_[a].name) < ToUpperInvariant(items_[b].name);
        });

        std::unordered_set<std::wstring> usedSlots;
        for (const auto& widget : widgets_)
            if (widget.gridCell.pageId == page.id)
                MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);

        int searchSlot = 0;
        for (size_t itemIndex : order)
        {
            items_[itemIndex].gridSpan = { 1, 1 };
            bool placed = false;
            for (int slot = searchSlot; slot < page.columns * page.rows; ++slot)
            {
                GridCell cell{ page.id, slot / std::max(1, page.rows), slot % std::max(1, page.rows) };
                if (cell.column >= page.columns || cell.row >= page.rows) continue;
                if (AreGridSlotsMarked(usedSlots, cell, items_[itemIndex].gridSpan)) continue;
                items_[itemIndex].gridCell = cell;
                items_[itemIndex].slot = cell.column * std::max(1, page.rows) + cell.row;
                MarkGridArea(usedSlots, cell, items_[itemIndex].gridSpan);
                searchSlot = slot + 1;
                placed = true;
                break;
            }
            if (!placed)
                MarkGridArea(usedSlots, items_[itemIndex].gridCell, items_[itemIndex].gridSpan);
        }
    };

    for (const auto& page : gridPages_)
        sortForPage(page);

    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 按类型名称对桌面图标排序，相同类型内按名称排序。
 */
inline void DesktopApp::SortIconsByType()
{
    auto sortForPage = [&](const GridPage& page) {
        std::vector<size_t> order;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].name.empty() || IsItemInAnyWidget(items_[i])) continue;
            if (items_[i].gridCell.pageId.empty())
                items_[i].gridCell.pageId = gridPages_.empty() ? L"" : gridPages_.front().id;
            if (items_[i].gridCell.pageId == page.id)
                order.push_back(i);
        }

        std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
            int cmp = ToUpperInvariant(items_[a].typeName).compare(ToUpperInvariant(items_[b].typeName));
            if (cmp != 0) return cmp < 0;
            return ToUpperInvariant(items_[a].name) < ToUpperInvariant(items_[b].name);
        });

        std::unordered_set<std::wstring> usedSlots;
        for (const auto& widget : widgets_)
            if (widget.gridCell.pageId == page.id)
                MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);

        int searchSlot = 0;
        for (size_t itemIndex : order)
        {
            items_[itemIndex].gridSpan = { 1, 1 };
            bool placed = false;
            for (int slot = searchSlot; slot < page.columns * page.rows; ++slot)
            {
                GridCell cell{ page.id, slot / std::max(1, page.rows), slot % std::max(1, page.rows) };
                if (cell.column >= page.columns || cell.row >= page.rows) continue;
                if (AreGridSlotsMarked(usedSlots, cell, items_[itemIndex].gridSpan)) continue;
                items_[itemIndex].gridCell = cell;
                items_[itemIndex].slot = cell.column * std::max(1, page.rows) + cell.row;
                MarkGridArea(usedSlots, cell, items_[itemIndex].gridSpan);
                searchSlot = slot + 1;
                placed = true;
                break;
            }
            if (!placed)
                MarkGridArea(usedSlots, items_[itemIndex].gridCell, items_[itemIndex].gridSpan);
        }
    };

    for (const auto& page : gridPages_)
        sortForPage(page);

    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 对指定组件（文件夹映射/桌面文件/集合）中的内容排序。
 * @param widgetIndex 组件索引。
 * @param mode 排序模式：0 按名称，1 按类型，2 按修改时间。
 */
inline void DesktopApp::SortWidgetContents(size_t widgetIndex, int mode)
{
    if (widgetIndex >= widgets_.size()) return;
    DesktopWidget& w = widgets_[widgetIndex];

    if (w.type == DesktopWidgetType::FolderMapping)
    {
        std::sort(w.folderEntries.begin(), w.folderEntries.end(),
            [mode](const FolderEntry& a, const FolderEntry& b) {
                if (a.isDirectory != b.isDirectory) return a.isDirectory;
                int cmp = 0;
                if (mode == 0) cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                else if (mode == 1)
                {
                    std::wstring extA = PathFindExtensionW(a.name.c_str());
                    std::wstring extB = PathFindExtensionW(b.name.c_str());
                    cmp = _wcsicmp(extA.c_str(), extB.c_str());
                    if (cmp == 0) cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                }
                else if (mode == 2)
                {
                    WIN32_FILE_ATTRIBUTE_DATA da{}, db{};
                    if (GetFileAttributesExW(a.fullPath.c_str(), GetFileExInfoStandard, &da) &&
                        GetFileAttributesExW(b.fullPath.c_str(), GetFileExInfoStandard, &db))
                    {
                        int timeCmp = CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime);
                        if (timeCmp != 0) return timeCmp < 0;
                    }
                    cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                }
                return cmp < 0;
            });
        w.itemKeys.clear();
        w.itemKeys.reserve(w.folderEntries.size());
        for (const auto& entry : w.folderEntries)
            w.itemKeys.push_back(entry.fullPath);
        RefreshFolderMappingWidget(widgetIndex);
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    else if (w.type == DesktopWidgetType::FileCategories)
    {
        std::vector<std::wstring> keys;
        std::unordered_set<std::wstring> seen;
        for (const auto& rawKey : w.itemKeys)
        {
            std::wstring nk = ToUpperInvariant(rawKey);
            if (seen.insert(nk).second)
                keys.push_back(rawKey);
        }

        std::sort(keys.begin(), keys.end(),
            [this, mode](const std::wstring& ka, const std::wstring& kb) {
                size_t ia = FindItemIndexByKey(ka);
                size_t ib = FindItemIndexByKey(kb);
                if (ia == static_cast<size_t>(-1) || ib == static_cast<size_t>(-1)) return false;
                int cmp = 0;
                if (mode == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                else if (mode == 1)
                {
                    cmp = _wcsicmp(items_[ia].typeName.c_str(), items_[ib].typeName.c_str());
                    if (cmp == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                else if (mode == 2)
                {
                    WIN32_FILE_ATTRIBUTE_DATA da{}, db{};
                    if (GetFileAttributesExW(items_[ia].parsingName.c_str(), GetFileExInfoStandard, &da) &&
                        GetFileAttributesExW(items_[ib].parsingName.c_str(), GetFileExInfoStandard, &db))
                    {
                        int timeCmp = CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime);
                        if (timeCmp != 0) return timeCmp < 0;
                    }
                    cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                return cmp < 0;
            });

        w.itemKeys = std::move(keys);
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    else if (w.type == DesktopWidgetType::Collection)
    {
        std::vector<std::wstring> keys;
        std::unordered_set<std::wstring> seen;
        for (const auto& rawKey : w.itemKeys)
        {
            std::wstring nk = ToUpperInvariant(rawKey);
            if (seen.insert(nk).second)
                keys.push_back(rawKey);
        }

        std::sort(keys.begin(), keys.end(),
            [this, mode](const std::wstring& ka, const std::wstring& kb) {
                size_t ia = FindItemIndexByKey(ka);
                size_t ib = FindItemIndexByKey(kb);
                if (ia == static_cast<size_t>(-1) || ib == static_cast<size_t>(-1)) return false;
                int cmp = 0;
                if (mode == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                else if (mode == 1)
                {
                    cmp = _wcsicmp(items_[ia].typeName.c_str(), items_[ib].typeName.c_str());
                    if (cmp == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                else if (mode == 2)
                {
                    WIN32_FILE_ATTRIBUTE_DATA da{}, db{};
                    if (GetFileAttributesExW(items_[ia].parsingName.c_str(), GetFileExInfoStandard, &da) &&
                        GetFileAttributesExW(items_[ib].parsingName.c_str(), GetFileExInfoStandard, &db))
                    {
                        int timeCmp = CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime);
                        if (timeCmp != 0) return timeCmp < 0;
                    }
                    cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                return cmp < 0;
            });

        w.itemKeys = std::move(keys);
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

/**
 * @brief 更新所有桌面项的剪切状态（从剪贴板读取 DROPEFFECT_MOVE）。
 */
inline void DesktopApp::UpdateCutState()
{
    std::unordered_set<std::wstring> clipCutPaths;

    ComPtr<IDataObject> clipObj;
    if (SUCCEEDED(OleGetClipboard(&clipObj)) && clipObj)
    {
        CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medPref{};
        bool isMove = false;

        if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) && medPref.hGlobal)
        {
            DWORD* pEffect = static_cast<DWORD*>(GlobalLock(medPref.hGlobal));
            if (pEffect)
            {
                if (*pEffect & DROPEFFECT_MOVE)
                    isMove = true;
                GlobalUnlock(medPref.hGlobal);
            }
            ReleaseStgMedium(&medPref);
        }

        if (isMove)
        {
            FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM medDrop{};
            if (SUCCEEDED(clipObj->GetData(&fmtDrop, &medDrop)) && medDrop.hGlobal)
            {
                HDROP hDrop = static_cast<HDROP>(medDrop.hGlobal);
                UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i)
                {
                    wchar_t path[MAX_PATH]{};
                    if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                        clipCutPaths.insert(ToUpperInvariant(path));
                }
                ReleaseStgMedium(&medDrop);
            }
        }
    }

    for (auto& item : items_)
    {
        item.isCut = false;
        if (item.desktopIconClsid.empty() == false) continue;
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
        {
            if (clipCutPaths.contains(ToUpperInvariant(path)))
                item.isCut = true;
        }
    }

    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        for (auto& entry : widget.folderEntries)
        {
            entry.isCut = false;
            if (!entry.fullPath.empty() &&
                clipCutPaths.contains(ToUpperInvariant(entry.fullPath)))
                entry.isCut = true;
        }
    }
}

// ── Shell 变更通知 ──────────────────────────────────────────

/**
 * @brief 注册 Shell 变更通知（文件创建、删除、重命名、属性变更等），用于实时刷新桌面。
 */
inline void DesktopApp::RegisterShellChangeNotifications()
{
    if (shellChangeRegId_ != 0)
    {
        SHChangeNotifyDeregister(shellChangeRegId_);
        shellChangeRegId_ = 0;
    }
    SHChangeNotifyEntry entries[2]{};
    entries[0].pidl = desktopPidl_.get();
    entries[0].fRecursive = FALSE;
    if (!recycleBinPidl_.get())
    {
        PIDLIST_ABSOLUTE rbPidl = nullptr;
        if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &rbPidl)))
            recycleBinPidl_.reset(rbPidl);
    }
    int entryCount = 1;
    if (recycleBinPidl_.get())
    {
        entries[1].pidl = recycleBinPidl_.get();
        entries[1].fRecursive = TRUE;
        entryCount = 2;
    }
    shellChangeRegId_ = SHChangeNotifyRegister(hwnd_,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
        SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
        SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES | SHCNE_ASSOCCHANGED,
        kShellChangeMessage, entryCount, entries);
}

// ── 过滤与键值 ───────────────────────────────────────────────

/**
 * @brief 获取稳定的布局键值，优先级：桌面图标 CLSID > 文件路径 > 解析名称。
 * @param pidl 绝对 PIDL。
 * @param parsingName 解析名称。
 * @param desktopIconClsid 桌面图标 CLSID。
 * @return 规范化为大写的布局键。
 */
inline std::wstring DesktopApp::GetStableLayoutKey(
    PCIDLIST_ABSOLUTE pidl,
    const std::wstring& parsingName,
    const std::wstring& desktopIconClsid)
{
    if (!desktopIconClsid.empty())
        return ToUpperInvariant(desktopIconClsid);

    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(pidl, path) && path[0] != L'\0')
        return ToUpperInvariant(path);

    return ToUpperInvariant(parsingName);
}

/**
 * @brief 给快捷方式的位图左下角绘制小箭头图标。
 * @param bitmap 目标位图。
 * @param bitmapSize 位图尺寸。
 */
inline void DesktopApp::ApplyShortcutArrowToBitmap(HBITMAP bitmap, SIZE bitmapSize)
{
    if (!bitmap) return;
    SHSTOCKICONINFO sii{};
    sii.cbSize = sizeof(sii);
    if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) || !sii.hIcon)
        return;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(hdc, bitmap));
    const int arrowSz = 30;
    DrawIconEx(hdc, 5, bitmapSize.cy - arrowSz, sii.hIcon, arrowSz, arrowSz, 0, nullptr, DI_NORMAL);
    SelectObject(hdc, oldBmp);
    DeleteDC(hdc);
    DestroyIcon(sii.hIcon);
}

// ── 布局持久化 ──────────────────────────────────────────────

/**
 * @brief 获取布局文件的完整路径（与 exe 同目录下的 SnowDesktop.layout.json）。
 * @return 布局文件路径。
 */
inline std::wstring DesktopApp::GetLayoutPath() const
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"SnowDesktop.layout.json");
    return path;
}

/**
 * @brief 从 JSON 文本中解析保存的页面信息（ID、行数、列数）。
 * @param text JSON 格式的布局文本。
 */
inline void DesktopApp::LoadSavedPagesFromJson(const std::string& text)
{
    size_t pagesName = text.find("\"pages\"");
    if (pagesName == std::string::npos) return;

    size_t arrayStart = text.find('[', pagesName);
    size_t arrayEnd = text.find(']', arrayStart == std::string::npos ? pagesName : arrayStart + 1);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos || arrayEnd <= arrayStart) return;

    size_t pos = arrayStart + 1;
    while ((pos = text.find('{', pos)) != std::string::npos && pos < arrayEnd)
    {
        size_t objectEnd = text.find('}', pos);
        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;

        std::string objectText = text.substr(pos, objectEnd - pos + 1);
        std::string pageUtf8;
        if (ReadJsonStringField(objectText, "id", pageUtf8))
        {
            std::wstring pageId = Utf8ToWide(pageUtf8);
            if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
                savedPageIds_.push_back(pageId);
            int columns = 0, rows = 0;
            if (ReadJsonIntField(objectText, "columns", columns) && columns > 0)
                savedPageColumns_[pageId] = columns;
            if (ReadJsonIntField(objectText, "rows", rows) && rows > 0)
                savedPageRows_[pageId] = rows;
        }
        pos = objectEnd + 1;
    }
}

/**
 * @brief 记录页面 ID 到已保存页面列表（去重）。
 * @param pageId 页面 ID。
 */
inline void DesktopApp::RememberSavedPageId(const std::wstring& pageId)
{
    if (pageId.empty()) return;
    if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
        savedPageIds_.push_back(pageId);
}

/**
 * @brief 从布局 JSON 文件加载所有页面、组件和项目的网格位置信息。
 *
 * 解析内容包括：首选监视器、页面 ID/行列数、每个项目的网格位置及组件定义。
 */
inline void DesktopApp::LoadLayoutSlots()
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    layoutRecords_.clear();
    widgets_.clear();
    savedPageIds_.clear();
    savedPageColumns_.clear();
    savedPageRows_.clear();

    std::ifstream file(GetLayoutPath(), std::ios::binary);
    if (!file) return;
    std::stringstream buf;
    buf << file.rdbuf();
    std::string text = buf.str();

    std::string firstPageMonitorUtf8;
    if (ReadJsonStringField(text, "firstPageMonitor", firstPageMonitorUtf8))
        firstPageMonitorId_ = Utf8ToWide(firstPageMonitorUtf8);

    LoadSavedPagesFromJson(text);

    size_t pos = 0;
    while ((pos = text.find("\"key\"", pos)) != std::string::npos)
    {
        size_t objStart = text.rfind('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = objStart + 1;
        int depth = 1;
        for (size_t i = objStart + 1; i < text.size() && depth > 0; ++i)
        {
            if (text[i] == '{') ++depth;
            else if (text[i] == '}') --depth;
            objEnd = i;
        }
        if (depth != 0) break;

        std::string objText = text.substr(objStart, objEnd - objStart + 1);
        std::string keyUtf8;
        if (!ReadJsonStringField(objText, "key", keyUtf8)) { pos = objEnd + 1; continue; }

        LayoutRecord record;
        std::string pageUtf8;
        int x = 0, y = 0, w = 1, h = 1;
        if (ReadJsonStringField(objText, "page", pageUtf8) &&
            ReadJsonIntField(objText, "x", x) && ReadJsonIntField(objText, "y", y))
        {
            record.cell.pageId = Utf8ToWide(pageUtf8);
            record.cell.column = x;
            record.cell.row = y;
            RememberSavedPageId(record.cell.pageId);
            ReadJsonIntField(objText, "w", w);
            ReadJsonIntField(objText, "h", h);
            record.span.columns = std::max(1, w);
            record.span.rows = std::max(1, h);
            record.hasGrid = true;
            record.legacySlot = SlotFromCell(gridPages_, record.cell);
        }
        layoutRecords_[ToUpperInvariant(Utf8ToWide(keyUtf8))] = record;
        pos = objEnd + 1;
    }

    // Load widgets
    {
        size_t widgetsName = text.find("\"widgets\"");
        if (widgetsName != std::string::npos)
        {
            size_t arrayStart = text.find('[', widgetsName);
            if (arrayStart != std::string::npos)
            {
                size_t arrayEnd = FindJsonArrayEnd(text, arrayStart);
                if (arrayEnd != std::string::npos && arrayEnd > arrayStart)
                {
                    size_t wp = arrayStart + 1;
                    while ((wp = text.find('{', wp)) != std::string::npos && wp < arrayEnd)
                    {
                        size_t objectEnd = FindJsonObjectEnd(text, wp);
                        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
                        std::string obj = text.substr(wp, objectEnd - wp + 1);
                        std::string idUtf8, typeUtf8, titleUtf8, sourceUtf8, scriptUtf8, activeCategoryUtf8, pageUtf8;
                        int x = 0, y = 0, w = 1, h = 1, scrollOffset = 0, tabScrollOffset = 0;
                        bool autoCollect = false, listMode = false;
                        if (!ReadJsonStringField(obj, "id", idUtf8) ||
                            !ReadJsonStringField(obj, "page", pageUtf8) ||
                            !ReadJsonIntField(obj, "x", x) ||
                            !ReadJsonIntField(obj, "y", y))
                        {
                            wp = objectEnd + 1;
                            continue;
                        }
                        ReadJsonStringField(obj, "type", typeUtf8);
                        ReadJsonStringField(obj, "title", titleUtf8);
                        ReadJsonStringField(obj, "sourceFolderPath", sourceUtf8);
                        ReadJsonStringField(obj, "scriptPath", scriptUtf8);
                        ReadJsonStringField(obj, "activeCategory", activeCategoryUtf8);
                        ReadJsonIntField(obj, "w", w);
                        ReadJsonIntField(obj, "h", h);
                        ReadJsonIntField(obj, "scrollOffset", scrollOffset);
ReadJsonIntField(obj, "tabScrollOffset", tabScrollOffset);
                        ReadJsonBoolField(obj, "autoCollect", autoCollect);
                        ReadJsonBoolField(obj, "listMode", listMode);

                        DesktopWidget widget;
                        widget.id = Utf8ToWide(idUtf8);
                        widget.type = WidgetTypeFromJson(Utf8ToWide(typeUtf8));
                        widget.sourceFolderPath = Utf8ToWide(sourceUtf8);
                        widget.scriptPath = Utf8ToWide(scriptUtf8);
                        if (titleUtf8.empty())
                        {
                            if (widget.type == DesktopWidgetType::LuaScript)
                            {
                                widget.title = WidgetEngine::GetWidgetDisplayName(widget.scriptPath);
                                if (widget.title.empty()) widget.title = widget.scriptPath;
                            }
                            else
                            {
                                widget.title = widget.type == DesktopWidgetType::FileCategories ? L"桌面文件"
                                    : widget.type == DesktopWidgetType::FolderMapping ? L"文件夹映射"
                                    : L"集合";
                            }
                        }
                        else
                        {
                            widget.title = Utf8ToWide(titleUtf8);
                        }
                        widget.gridCell.pageId = Utf8ToWide(pageUtf8);
                        widget.gridCell.column = x;
                        widget.gridCell.row = y;
                        widget.gridSpan.columns = std::max(1, w);
                        widget.gridSpan.rows = std::max(1, h);
                        widget.autoCollect = autoCollect;
                        widget.listMode = listMode;
                        widget.showTitle = widget.type != DesktopWidgetType::LuaScript;
                        widget.bottomBarHover = (widget.type == DesktopWidgetType::Collection ||
                            widget.type == DesktopWidgetType::LuaScript);
                        widget.scrollOffset = std::max(0, scrollOffset);
widget.tabScrollOffset = std::max(0, tabScrollOffset);
                        widget.activeCategoryId = Utf8ToWide(activeCategoryUtf8);
                        ReadJsonStringArrayField(obj, "items", widget.itemKeys);
                        ConfigureWidgetGridLimits(widget);
                        {
                            std::unordered_set<std::wstring> seen;
                            std::vector<std::wstring> unique;
                            for (auto& key : widget.itemKeys)
                            {
                                key = ToUpperInvariant(key);
                                if (!key.empty() && seen.insert(key).second)
                                    unique.push_back(key);
                            }
                            widget.itemKeys = std::move(unique);
                        }

                        widgets_.push_back(std::move(widget));
                        if (widgets_.back().type == DesktopWidgetType::FolderMapping &&
                            !widgets_.back().sourceFolderPath.empty())
                        {
                            EnumerateFolderMappingEntries(widgets_.back());
                        }
                        wp = objectEnd + 1;
                    }
                }
            }
        }
    }

    // Ensure widget-owned items have layout records (they're not in the JSON items array)
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    for (auto& w : widgets_)
    {
        for (auto& key : w.itemKeys)
        {
            auto upper = ToUpperInvariant(key);
            if (layoutRecords_.count(upper) == 0)
            {
                LayoutRecord rec;
                rec.cell = w.gridCell;
                rec.span = {1, 1};
                rec.hasGrid = true;
                rec.legacySlot = SlotFromCell(gridPages_, w.gridCell);
                layoutRecords_[upper] = rec;
            }
        }
    }

    {
        std::vector<std::wstring> savedOrder;
        ReadJsonStringArrayField(text, "navTabOrder", savedOrder);
        navTabOrder_ = std::move(savedOrder);
    }
    EnsureNavTabOrder();
}

/**
 * @brief 将所有项目、组件和页面的网格布局信息持久化到 JSON 文件。
 *
 * 写入内容包括：首选监视器、页面列表、桌面项（排除组件所属项）以及所有组件的完整定义。
 */
inline void DesktopApp::SaveLayoutSlots()
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    layoutRecords_.clear();
    for (const auto& item : items_)
    {
        if (!item.parsingName.empty())
        {
            RememberSavedPageId(item.gridCell.pageId);
            LayoutRecord record;
            record.cell = item.gridCell;
            record.span = item.gridSpan;
            record.hasGrid = true;
            record.legacySlot = item.slot;
            layoutRecords_[item.layoutKey] = record;
        }
    }

    std::vector<const DesktopItem*> sorted;
    for (const auto& item : items_) sorted.push_back(&item);
    std::sort(sorted.begin(), sorted.end(), [](const DesktopItem* a, const DesktopItem* b) {
        if (a->gridCell.pageId != b->gridCell.pageId) return a->gridCell.pageId < b->gridCell.pageId;
        if (a->gridCell.column != b->gridCell.column) return a->gridCell.column < b->gridCell.column;
        return a->gridCell.row < b->gridCell.row;
    });

    for (const auto& page : gridPages_)
    {
        savedPageColumns_[page.id] = page.columns;
        savedPageRows_[page.id] = page.rows;
    }

    std::vector<std::wstring> pagesToWrite = savedPageIds_;
    if (pagesToWrite.empty() && !gridPages_.empty())
        pagesToWrite.push_back(gridPages_.front().id);

    std::ofstream file(GetLayoutPath(), std::ios::binary | std::ios::trunc);
    if (!file) return;

    file << "{\n  \"firstPageMonitor\": \"" << JsonEscapeUtf8(firstPageMonitorId_) << "\",\n  \"pages\": [\n";
    for (size_t i = 0; i < pagesToWrite.size(); ++i)
    {
        const GridPage* page = FindGridPage(gridPages_, pagesToWrite[i]);
        file << "    { \"id\": \"" << JsonEscapeUtf8(pagesToWrite[i]) << "\", \"monitor\": \"";
        file << JsonEscapeUtf8(page != nullptr ? page->monitorId : L"");
        int columns = page != nullptr ? page->columns : 0;
        int rows = page != nullptr ? page->rows : 0;
        if (page == nullptr)
        {
            auto colIt = savedPageColumns_.find(pagesToWrite[i]);
            auto rowIt = savedPageRows_.find(pagesToWrite[i]);
            if (colIt != savedPageColumns_.end()) columns = colIt->second;
            if (rowIt != savedPageRows_.end()) rows = rowIt->second;
        }
        file << "\", \"columns\": " << std::max(2, columns) <<
            ", \"rows\": " << std::max(2, rows) << " }";
        file << (i + 1 == pagesToWrite.size() ? "\n" : ",\n");
    }
    file << "  ],\n  \"items\": [\n";
    // Collect widget-owned keys — items in widgets should not be saved
    // to the desktop items array (they belong to their widget's items list)
    std::unordered_set<std::wstring> widgetOwnedKeys;
    for (auto& w : widgets_)
        for (auto& k : w.itemKeys)
            if (!k.empty())
                widgetOwnedKeys.insert(ToUpperInvariant(k));

    bool firstItem = true;
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto* it = sorted[i];
        if (widgetOwnedKeys.count(ToUpperInvariant(it->layoutKey))) continue;
        if (!firstItem) file << ",\n";
        firstItem = false;
        file << "    { \"key\": \"" << JsonEscapeUtf8(it->layoutKey)
             << "\", \"page\": \"" << JsonEscapeUtf8(it->gridCell.pageId)
             << "\", \"x\": " << it->gridCell.column
             << ", \"y\": " << it->gridCell.row
             << ", \"w\": " << std::max(1, it->gridSpan.columns)
             << ", \"h\": " << std::max(1, it->gridSpan.rows)
             << ", \"slot\": " << it->slot << " }";
    }
    if (!firstItem) file << "\n";
    file << "  ],\n  \"widgets\": [\n";
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& w = widgets_[i];
        file << "    { \"id\": \"" << JsonEscapeUtf8(w.id)
             << "\", \"type\": \"" << JsonEscapeUtf8(WidgetTypeToJson(w.type))
             << "\", \"title\": \"" << JsonEscapeUtf8(w.title)
             << "\", \"sourceFolderPath\": \"" << JsonEscapeUtf8(w.sourceFolderPath)
             << "\", \"scriptPath\": \"" << JsonEscapeUtf8(w.scriptPath)
             << "\", \"activeCategory\": \"" << JsonEscapeUtf8(w.activeCategoryId)
             << "\", \"page\": \"" << JsonEscapeUtf8(w.gridCell.pageId)
             << "\", \"x\": " << w.gridCell.column
             << ", \"y\": " << w.gridCell.row
             << ", \"w\": " << std::max(1, w.gridSpan.columns)
             << ", \"h\": " << std::max(1, w.gridSpan.rows)
             << ", \"autoCollect\": " << (w.autoCollect ? "true" : "false")
             << ", \"listMode\": " << (w.listMode ? "true" : "false")
             << ", \"scrollOffset\": " << std::max(0, w.scrollOffset)
             << ", \"tabScrollOffset\": " << std::max(0, w.tabScrollOffset)
             << ", \"items\": [";
        for (size_t j = 0; j < w.itemKeys.size(); ++j)
        {
            file << "\"" << JsonEscapeUtf8(w.itemKeys[j]) << "\"";
            if (j + 1 != w.itemKeys.size()) file << ", ";
        }
        file << "] }";
        file << (i + 1 == widgets_.size() ? "\n" : ",\n");
    }
    file << "  ],\n  \"navTabOrder\": [";
    for (size_t i = 0; i < navTabOrder_.size(); ++i)
    {
        file << "\"" << JsonEscapeUtf8(navTabOrder_[i]) << "\"";
        if (i + 1 != navTabOrder_.size()) file << ", ";
    }
    file << "]\n}\n";
}

/**
 * @brief 从 JSON 对象文本中读取字符串字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，UTF-8 编码的值。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t quote = objectText.find('"', colon == std::string::npos ? name + marker.size() : colon + 1);
    size_t end = 0;
    return quote != std::string::npos && ParseJsonStringAt(objectText, quote, value, end);
}

/**
 * @brief 从 JSON 对象文本中读取整数字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，整数值。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t numberStart = objectText.find_first_of("-0123456789", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (numberStart == std::string::npos) return false;
    try { value = std::stoi(objectText.substr(numberStart)); return true; }
    catch (...) { return false; }
}

/**
 * @brief 从 JSON 对象文本中读取布尔字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，布尔值。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonBoolField(const std::string& objectText, const char* fieldName, bool& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t valueStart = objectText.find_first_not_of(" \t\r\n", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (valueStart == std::string::npos) return false;
    if (objectText.compare(valueStart, 4, "true") == 0) { value = true; return true; }
    if (objectText.compare(valueStart, 5, "false") == 0) { value = false; return true; }
    return false;
}

/**
 * @brief 在 JSON 文本中查找匹配的闭合括号位置（支持字符串内转义）。
 * @param text JSON 文本。
 * @param start 起始位置（应为 '{' 或 '['）。
 * @param open 起始括号字符。
 * @param close 闭合括号字符。
 * @return 闭合位置，未找到返回 npos。
 */
inline size_t DesktopApp::FindJsonContainerEnd(const std::string& text, size_t start, char open, char close) const
{
    if (start >= text.size() || text[start] != open) return std::string::npos;
    int depth = 1;
    bool inString = false;
    for (size_t i = start + 1; i < text.size(); ++i)
    {
        char ch = text[i];
        if (ch == '"' && (i == 0 || text[i - 1] != '\\')) inString = !inString;
        else if (!inString)
        {
            if (ch == open) ++depth;
            else if (ch == close) { --depth; if (depth == 0) return i; }
        }
    }
    return std::string::npos;
}

/**
 * @brief 在 JSON 文本中查找对象结束位置。
 */
inline size_t DesktopApp::FindJsonObjectEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '{', '}'); }

/**
 * @brief 在 JSON 文本中查找数组结束位置。
 */
inline size_t DesktopApp::FindJsonArrayEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '[', ']'); }

/**
 * @brief 从 JSON 对象文本中读取字符串数组字段。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param values 输出参数，宽字符串数组。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonStringArrayField(const std::string& objectText, const char* fieldName, std::vector<std::wstring>& values) const
{
    values.clear();
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t arrayStart = objectText.find('[', colon == std::string::npos ? name + marker.size() : colon + 1);
    if (arrayStart == std::string::npos) return false;
    size_t arrayEnd = FindJsonArrayEnd(objectText, arrayStart);
    if (arrayEnd == std::string::npos) return false;
    size_t pos = arrayStart + 1;
    while (pos < arrayEnd)
    {
        size_t quote = objectText.find('"', pos);
        if (quote == std::string::npos || quote >= arrayEnd) break;
        std::string utf8;
        size_t end = 0;
        if (!ParseJsonStringAt(objectText, quote, utf8, end)) break;
        values.push_back(Utf8ToWide(utf8));
        pos = end;
    }
    return true;
}

/**
 * @brief 将 JSON 字符串转换为组件类型枚举。
 * @param type 类型字符串（不区分大小写）。
 * @return 对应的 DesktopWidgetType 枚举值。
 */
inline DesktopWidgetType DesktopApp::WidgetTypeFromJson(const std::wstring& type) const
{
    std::wstring n = ToUpperInvariant(type);
    if (n == L"FILECATEGORIES" || n == L"FILE_CATEGORIES") return DesktopWidgetType::FileCategories;
    if (n == L"FOLDERMAPPING" || n == L"FOLDER_MAPPING") return DesktopWidgetType::FolderMapping;
    if (n == L"LUA" || n == L"LUASCRIPT" || n == L"LUA_SCRIPT") return DesktopWidgetType::LuaScript;
    if (n == L"COLLECTION") return DesktopWidgetType::Collection;
    return DesktopWidgetType::Collection;
}

/**
 * @brief 将组件类型枚举转换为 JSON 字符串。
 * @param type 组件类型。
 * @return 对应的字符串表示。
 */
inline std::wstring DesktopApp::WidgetTypeToJson(DesktopWidgetType type) const
{
    switch (type)
    {
    case DesktopWidgetType::FileCategories: return L"fileCategories";
    case DesktopWidgetType::FolderMapping:  return L"folderMapping";
    case DesktopWidgetType::LuaScript:      return L"lua";
    case DesktopWidgetType::Collection:
    default:                                return L"collection";
    }
}

// ── 控件窗口 ──────────────────────────────────────────

/**
 * @brief 控件窗口的消息处理函数（静态回调），将消息转发到 HandleControlMessage。
 * @param hwnd 窗口句柄。
 * @param msg 消息 ID。
 * @param wp wParam。
 * @param lp lParam。
 * @return 消息处理结果。
 */
inline LRESULT CALLBACK DesktopApp::ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (app) return app->HandleControlMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 处理控件窗口的消息：任务栏重启、托盘回调、定时器、命令、关闭、销毁等。
 * @param hwnd 窗口句柄。
 * @param msg 消息 ID。
 * @param wp wParam。
 * @param lp lParam。
 * @return 消息处理结果。
 */
inline LRESULT DesktopApp::HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (taskbarRestartMsg_ && msg == taskbarRestartMsg_)
    {
        RecoverDesktopHostAfterExplorerRestart();
        return 0;
    }
    switch (msg)
    {
    case kTrayCallbackMessage:
        OnTrayCallback(lp);
        return 0;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case WM_COMMAND:
        return 0;
    case WM_CLOSE:
        RequestExit();
        return 0;
    case WM_DESTROY:
        controlHwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 位图缓存 ────────────────────────────────────────────

/**
 * @brief 将半透明暗色像素（alpha < 250 且 RGB 和 < 150）的 alpha 钳制为 0。
 * @param bitmap 32 位 DIB 位图句柄。
 * @param key 颜色键（未使用，保留参数）。
 */
inline void ClampAlphaToColorKey(HBITMAP bitmap, COLORREF key)
{
    if (!bitmap) return;
    BITMAP bm{};
    if (GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmBitsPixel != 32 || !bm.bmBits) return;
    int w = bm.bmWidth, absH = std::abs(bm.bmHeight);
    auto* pixels = static_cast<std::uint32_t*>(bm.bmBits);
    size_t count = static_cast<size_t>(w) * static_cast<size_t>(absH);
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t a = (pixels[i] >> 24) & 0xff;
        uint8_t r = (pixels[i] >> 16) & 0xff;
        uint8_t g = (pixels[i] >> 8)  & 0xff;
        uint8_t b = pixels[i] & 0xff;
        if (a < 250 && (int(r) + int(g) + int(b)) < 150)
            pixels[i] = 0;
    }
    (void)key;
}

/**
 * @brief 重新加载桌面项，可选择是否重新从磁盘读取布局。
 * @param reloadLayoutFromDisk 是否重新加载布局文件。
 */
inline void DesktopApp::ReloadItems(bool reloadLayoutFromDisk)
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    if (reloading_) return;
    reloading_ = true;
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    if (reloadLayoutFromDisk)
    {
        LoadLayoutSlots();
        ApplyPageMapping();
        if (widgetEngine_)
            widgetEngine_->ReloadStorage();
    }
    LoadDesktopItems();
    ApplyAutoCollectFileCategoryWidgets();

    // Mark widgets as used
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    // Mark items with valid existing positions as used; flag unslotted items
    std::unordered_set<std::wstring> placedKeys;
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (!gridPages_.empty() && item.gridCell.pageId.empty())
            item.gridCell.pageId = gridPages_.front().id;

        auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (page == nullptr)
        {
            // Item belongs to a page not currently visible — preserve its grid slot
            continue;
        }

        bool validSlot = page != nullptr &&
            item.gridCell.column + item.gridSpan.columns <= page->columns &&
            item.gridCell.row + item.gridSpan.rows <= page->rows &&
            !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan) &&
            !placedKeys.contains(item.layoutKey);

        if (validSlot)
        {
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
            placedKeys.insert(item.layoutKey);
        }
        else
        {
            item.gridCell = {};
            item.gridSpan = {1, 1};
        }
    }

    // Assign free cells to unslotted items
    std::vector<DesktopItem*> unslotted;
    for (auto& item : items_)
    {
        if (!item.name.empty() && !IsItemInAnyWidget(item) && item.gridCell.pageId.empty())
            unslotted.push_back(&item);
    }

    std::sort(unslotted.begin(), unslotted.end(), [](const DesktopItem* a, const DesktopItem* b) {
        bool aDesk = !a->desktopIconClsid.empty();
        bool bDesk = !b->desktopIconClsid.empty();
        if (aDesk != bDesk) return aDesk;
        return ToUpperInvariant(a->name) < ToUpperInvariant(b->name);
    });

    for (auto* item : unslotted)
    {
        GridCell freeCell;
        if (TryFindFreeCell(item->gridSpan, usedSlots, freeCell))
        {
            item->gridCell = freeCell;
            MarkGridArea(usedSlots, freeCell, item->gridSpan);
        }
        else if (!gridPages_.empty())
        {
            item->gridCell.pageId = gridPages_.front().id;
        }
    }

    LayoutItems();
    ApplyPendingPlacement();
    UpdateCutState();

    // Prune desktop-backed widget itemKeys that no longer exist (file was deleted from outside).
    // FolderMapping keys are mapped-folder paths, not desktop layout keys.
    std::unordered_set<std::wstring> allKeys;
    for (auto& item : items_)
        if (!item.layoutKey.empty())
            allKeys.insert(ToUpperInvariant(item.layoutKey));
    for (auto& w : widgets_)
    {
        if (w.type == DesktopWidgetType::FolderMapping)
            continue;
        auto it = std::remove_if(w.itemKeys.begin(), w.itemKeys.end(),
            [&](const std::wstring& key) {
                return allKeys.count(ToUpperInvariant(key)) == 0;
            });
        w.itemKeys.erase(it, w.itemKeys.end());
    }

    SaveLayoutSlots();
    RebuildContainersAndItems();
    reloading_ = false;
    if (widgetEngine_)
        widgetEngine_->NotifyDesktopChanged("reload");
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 枚举桌面文件夹中的所有项，构建 DesktopItem 列表，包含图标、布局键和网格位置。
 *
 * 会过滤隐藏项和非桌面路径项，并为 .lnk 文件检测快捷方式箭头。
 */
inline void DesktopApp::LoadDesktopItems()
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    auto L = [](const wchar_t* s) {
        HANDLE f = CreateFileW(L"SnowDesktop_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, s, static_cast<DWORD>(wcslen(s)*2), &w, nullptr);
            WriteFile(f, L"\r\n", 4, &w, nullptr); CloseHandle(f); }
    };

    items_.clear();
    d2dIconCache_.clear();
    L(L"LoadItems start");

    HRESULT hr = SHGetDesktopFolder(&desktopFolder_);
    if (FAILED(hr) || !desktopFolder_) { L(L"SHGetDesktopFolder FAILED"); return; }
    L(L"DesktopFolder ok");

    LPITEMIDLIST raw = nullptr;
    hr = SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &raw);
    if (FAILED(hr) || !raw) { L(L"SHGetSpecialFolderLocation FAILED"); return; }
    desktopPidl_.reset(raw);
    L(L"DesktopPidl ok");

    wchar_t userDesktopPath[MAX_PATH]{};
    wchar_t commonDesktopPath[MAX_PATH]{};
    wchar_t userProfilePath[MAX_PATH]{};
    SHGetSpecialFolderPathW(nullptr, userDesktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
    SHGetSpecialFolderPathW(nullptr, commonDesktopPath, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
    SHGetSpecialFolderPathW(nullptr, userProfilePath, CSIDL_PROFILE, FALSE);
    size_t userDesktopLen = wcslen(userDesktopPath);
    size_t commonDesktopLen = wcslen(commonDesktopPath);

    ComPtr<IEnumIDList> enumerator;
    hr = desktopFolder_->EnumObjects(hwnd_, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enumerator);
    if (FAILED(hr) || !enumerator) { L(L"EnumObjects FAILED"); return; }
    L(L"EnumObjects ok");

    PITEMID_CHILD child = nullptr;
    ULONG fetched = 0;
    int count = 0;
    wchar_t buf[64];
    std::unordered_set<std::wstring> seenKeys;
    while (enumerator->Next(1, &child, &fetched) == S_OK)
    {
        PIDLIST_ABSOLUTE absolute = ILCombine(desktopPidl_.get(), child);
        if (!absolute) { ILFree(child); continue; }

        // Get parsing name (used for CLSID detection)
        std::wstring parsingName = StrRetToString(
            desktopFolder_.Get(), reinterpret_cast<PCUITEMID_CHILD>(child), SHGDN_FORPARSING);

        // Get file system path
        wchar_t itemPath[MAX_PATH]{};
        std::wstring itemPathStr;
        if (SHGetPathFromIDListW(absolute, itemPath) && itemPath[0])
            itemPathStr = itemPath;

        std::wstring clsid = ResolveDesktopIconClsid(parsingName, itemPathStr, userProfilePath);
        bool isDesktopIcon = !clsid.empty();

        // Non-desktop-icon: check hidden
        if (!isDesktopIcon)
        {
            SFGAOF attrs = SFGAO_HIDDEN | SFGAO_NONENUMERATED;
            LPCITEMIDLIST childConst = child;
            if (SUCCEEDED(desktopFolder_->GetAttributesOf(1, &childConst, &attrs)))
            {
                if ((attrs & SFGAO_HIDDEN) || (attrs & SFGAO_NONENUMERATED))
                { ILFree(absolute); ILFree(child); continue; }
            }
        }

        // Get display name and icon
        SHFILEINFOW info{};
        SHGetFileInfoW(reinterpret_cast<LPCWSTR>(absolute), 0, &info, sizeof(info),
            SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME | SHGFI_TYPENAME);

        // Check visibility (applies to all items)
        if (!IsVisibleByDesktopIconSettings(clsid, settingsIconVisibility_))
        { ILFree(absolute); ILFree(child); continue; }

        // Non-desktop-icon: must be physically on desktop
        if (!isDesktopIcon && !itemPathStr.empty())
        {
            bool underUser = itemPathStr.size() > userDesktopLen &&
                _wcsnicmp(itemPathStr.c_str(), userDesktopPath, userDesktopLen) == 0 &&
                itemPathStr[userDesktopLen] == L'\\';
            bool underCommon = itemPathStr.size() > commonDesktopLen &&
                _wcsnicmp(itemPathStr.c_str(), commonDesktopPath, commonDesktopLen) == 0 &&
                itemPathStr[commonDesktopLen] == L'\\';
            if (!underUser && !underCommon)
            { ILFree(absolute); ILFree(child); continue; }
        }

        DesktopItem item;
        item.absolutePidl.reset(absolute);
        item.childPidl.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(child));
        item.parsingName = std::move(parsingName);
        item.desktopIconClsid = std::move(clsid);
        item.name = info.szDisplayName[0] ? info.szDisplayName
            : StrRetToString(desktopFolder_.Get(), reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()), SHGDN_NORMAL);
        item.typeName = info.szTypeName;
        item.sysIconIndex = info.iIcon;
        item.iconBitmap = GetHighResolutionShellIconBitmap(item.absolutePidl.get(), info.iIcon, item.iconBitmapSize);
        ClampAlphaToColorKey(item.iconBitmap, kTransparentKey);

        // .lnk shortcut arrow detection
        item.shortcutArrow = false;
        {
            std::wstring upper = item.parsingName;
            for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
            if (upper.size() > 4 && upper.compare(upper.size() - 4, 4, L".LNK") == 0)
            {
                wchar_t lnkPath[MAX_PATH]{};
                if (SHGetPathFromIDListW(item.absolutePidl.get(), lnkPath))
                {
                    ComPtr<IShellLinkW> shellLink;
                    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                    {
                        ComPtr<IPersistFile> persistFile;
                        if (SUCCEEDED(shellLink.As(&persistFile)) &&
                            SUCCEEDED(persistFile->Load(lnkPath, STGM_READ)))
                        {
                            wchar_t target[MAX_PATH]{};
                            if (SUCCEEDED(shellLink->GetPath(target, MAX_PATH, nullptr, 0)))
                            {
                                std::wstring t(target);
                                for (auto& c : t) c = static_cast<wchar_t>(towupper(c));
                                if (t.size() < 4 || t.compare(t.size() - 4, 4, L".EXE") != 0)
                                    item.shortcutArrow = true;
                            }
                        }
                    }
                }
            }
        }
        if (item.shortcutArrow && item.iconBitmap)
        {
            ApplyShortcutArrowToBitmap(item.iconBitmap, item.iconBitmapSize);
        }

        item.layoutKey = GetStableLayoutKey(item.absolutePidl.get(), item.parsingName, item.desktopIconClsid);

        if (seenKeys.contains(item.layoutKey))
        { ILFree(absolute); ILFree(child); continue; }
        seenKeys.insert(item.layoutKey);

        auto knownRecord = layoutRecords_.find(item.layoutKey);
        if (knownRecord != layoutRecords_.end() && knownRecord->second.hasGrid)
        {
            item.gridCell = knownRecord->second.cell;
            item.gridSpan = knownRecord->second.span;
            item.slot = SlotFromCell(gridPages_, item.gridCell);
        }
        else
        {
            item.gridCell = {};
            item.gridSpan = {1, 1};
            item.slot = -1;
        }

        items_.push_back(std::move(item));
        ++count;
    }
    // child PIDL ownership transferred to last DesktopItem — do NOT ILFree

    wsprintfW(buf, L"Loaded %d items", count);
    L(buf);
}

/**
 * @brief 更新布局工作区，枚举显示器并创建对应 GridPage，然后应用页面映射。
 */
inline void DesktopApp::UpdateLayoutWorkArea()
{
    layoutWorkArea_ = MakeRect(0, 0, virtualWidth_, virtualHeight_);
    gridPages_.clear();

    MonitorEnumContext ctx{};
    ctx.virtualLeft = virtualLeft_;
    ctx.virtualTop = virtualTop_;
    ctx.pages = &gridPages_;
    EnumDisplayMonitors(nullptr, nullptr, EnumGridPageMonitorProc, reinterpret_cast<LPARAM>(&ctx));

    if (gridPages_.empty())
    {
        GridPage fb;
        fb.id = L"Primary"; fb.monitorId = fb.id; fb.isPrimary = true;
        fb.bounds = layoutWorkArea_; fb.workArea = layoutWorkArea_;
        gridPages_.push_back(fb);
    }

    std::sort(gridPages_.begin(), gridPages_.end(), [](const GridPage& a, const GridPage& b) {
        return a.bounds.left < b.bounds.left;
    });

    for (auto& page : gridPages_)
    {
        page.workArea.left   = std::clamp<LONG>(page.workArea.left,   0, static_cast<LONG>(virtualWidth_));
        page.workArea.top    = std::clamp<LONG>(page.workArea.top,    0, static_cast<LONG>(virtualHeight_));
        page.workArea.right  = std::clamp<LONG>(page.workArea.right,  page.workArea.left, static_cast<LONG>(virtualWidth_));
        page.workArea.bottom = std::clamp<LONG>(page.workArea.bottom, page.workArea.top,  static_cast<LONG>(virtualHeight_));
        ConfigureGridPage(page);
    }

    ApplyPageMapping();
}

/**
 * @brief 根据工作区尺寸和缩放比例配置网格页面的列数、行数、单元格尺寸和间距。
 * @param page 待配置的网格页面。
 */
inline void DesktopApp::ConfigureGridPage(GridPage& page) const
{
    const float dpiScaleX = static_cast<float>(page.dpiX) / 96.0f;
    const float dpiScaleY = static_cast<float>(page.dpiY) / 96.0f;
    const int marginX = std::max(1, static_cast<int>(std::round(kGridMarginX * dpiScaleX)));
    const int marginY = std::max(1, static_cast<int>(std::round(kGridMarginY * dpiScaleY)));
    const int cw = std::max(1, static_cast<int>(std::round(kCellWidth * gapScale_ * dpiScaleX)));
    const int ch = std::max(1, static_cast<int>(std::round(kMinCellHeight * gapScale_ * dpiScaleY)));
    const int w  = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
    const int h  = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
    const int uw = std::max(1, w - marginX * 2);
    const int uh = std::max(1, h - marginY * 2);
    page.columns   = std::max(4, uw / cw);
    page.rows      = std::max(3, uh / ch);
    page.cellWidth  = cw;
    page.cellHeight = ch;
    page.gapX = page.columns > 1
        ? std::max(0, (uw - page.columns * page.cellWidth + (page.columns - 1) / 2) / (page.columns - 1))
        : 0;
    page.gapY = page.rows > 1
        ? std::max(0, (uh - page.rows * page.cellHeight + (page.rows - 1) / 2) / (page.rows - 1))
        : 0;
    // Absorb half-gap into margins so widget frames have enough edge room
    page.marginX = marginX + std::max(2, page.gapX / 2);
    page.marginY = marginY + std::max(2, page.gapY / 2);
    // Recompute gaps with the enlarged margins to keep values consistent
    const int uw2 = std::max(1, w - page.marginX * 2);
    const int uh2 = std::max(1, h - page.marginY * 2);
    page.gapX = page.columns > 1
        ? std::max(0, (uw2 - page.columns * page.cellWidth + (page.columns - 1) / 2) / (page.columns - 1))
        : 0;
    page.gapY = page.rows > 1
        ? std::max(0, (uh2 - page.rows * page.cellHeight + (page.rows - 1) / 2) / (page.rows - 1))
        : 0;
}

/**
 * @brief 将保存的列/行数设置应用到当前网格页面。
 */
inline void DesktopApp::ApplySavedGridDimensions()
{
    for (auto& page : gridPages_)
    {
        auto colIt = savedPageColumns_.find(page.id);
        auto rowIt = savedPageRows_.find(page.id);
        if (colIt != savedPageColumns_.end() && rowIt != savedPageRows_.end() &&
            colIt->second >= 2 && rowIt->second >= 2)
        {
            page.columns = colIt->second;
            page.rows = rowIt->second;
            ApplyGapScaleToPage(page);
        }
    }
}

/**
 * @brief 根据缩放比例重新计算页面的单元格尺寸和间距。
 * @param page 目标网格页面。
 */
inline void DesktopApp::ApplyGapScaleToPage(GridPage& page)
{
    const float dpiScaleX = static_cast<float>(page.dpiX) / 96.0f;
    const float dpiScaleY = static_cast<float>(page.dpiY) / 96.0f;
    const int marginX = std::max(1, static_cast<int>(std::round(kGridMarginX * dpiScaleX)));
    const int marginY = std::max(1, static_cast<int>(std::round(kGridMarginY * dpiScaleY)));
    const int pageW = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
    const int pageH = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
    const int usableW = std::max(1, pageW - marginX * 2);
    const int usableH = std::max(1, pageH - marginY * 2);
    const float cellRefW = static_cast<float>(usableW) / static_cast<float>(std::max(1, page.columns));
    const float cellRefH = static_cast<float>(usableH) / static_cast<float>(std::max(1, page.rows));
    const int targetGapX = std::max(0, static_cast<int>(cellRefW * kGapPercentX / gapScale_));
    const int targetGapY = std::max(0, static_cast<int>(cellRefH * kGapPercentY / gapScale_));

    const int minIconWidth = std::max(1, static_cast<int>(std::round(kIconSize * dpiScaleX)));
    const int minCellHeight = std::max(1, static_cast<int>(std::round((kMinCellHeight / 2.0f) * dpiScaleY)));
    page.cellWidth = page.columns > 1
        ? std::max(minIconWidth, (usableW - (page.columns - 1) * targetGapX) / page.columns)
        : usableW;
    page.cellHeight = page.rows > 1
        ? std::max(minCellHeight, (usableH - (page.rows - 1) * targetGapY) / page.rows)
        : usableH;
    // Prevent overflow when saved columns/rows exceed usable area
    if (page.columns > 1)
    {
        if (page.columns * page.cellWidth > usableW)
        {
            int maxFit = std::max(1, usableW / minIconWidth);
            if (maxFit < page.columns)
                page.columns = std::max(4, maxFit);
            page.cellWidth = std::max(minIconWidth, usableW / page.columns);
        }
    }
    if (page.rows > 1)
    {
        if (page.rows * page.cellHeight > usableH)
        {
            int maxFit = std::max(1, usableH / minCellHeight);
            if (maxFit < page.rows)
                page.rows = std::max(3, maxFit);
            page.cellHeight = std::max(minCellHeight, usableH / page.rows);
        }
    }
    page.gapX = page.columns > 1
        ? std::max(0, (usableW - page.columns * page.cellWidth + (page.columns - 1) / 2) / (page.columns - 1))
        : 0;
    page.gapY = page.rows > 1
        ? std::max(0, (usableH - page.rows * page.cellHeight + (page.rows - 1) / 2) / (page.rows - 1))
        : 0;
    // Absorb half-gap into margins so widget frames have enough edge room
    page.marginX = marginX + std::max(2, page.gapX / 2);
    page.marginY = marginY + std::max(2, page.gapY / 2);
    // Recompute gaps with the enlarged margins to keep values consistent
    const int usableW2 = std::max(1, pageW - page.marginX * 2);
    const int usableH2 = std::max(1, pageH - page.marginY * 2);
    page.gapX = page.columns > 1
        ? std::max(0, (usableW2 - page.columns * page.cellWidth + (page.columns - 1) / 2) / (page.columns - 1))
        : 0;
    page.gapY = page.rows > 1
        ? std::max(0, (usableH2 - page.rows * page.cellHeight + (page.rows - 1) / 2) / (page.rows - 1))
        : 0;
}

// ── 拖拽辅助函数 ──────────────────────────────────────────────

extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
extern inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal);
extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);
extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);

/**
 * @brief 根据坐标计算网格轴上的索引位置（采用最近距离法）。
 * @param page 网格页面。
 * @param coordinate 像素坐标。
 * @param horizontal true 为水平轴，false 为垂直轴。
 * @return 最近的轴索引。
 */
inline int DesktopApp::GetGridAxisIndexFromPoint(const GridPage& page, int coordinate, bool horizontal)
{
    const int count = horizontal ? page.columns : page.rows;
    if (count <= 1) return 0;
    int bestIndex = 0;
    int bestDistance = INT_MAX;
    for (int i = 0; i < count; ++i)
    {
        const int left = (horizontal ? page.workArea.left : page.workArea.top) +
            (horizontal ? page.marginX : page.marginY) + GetGridAxisOffset(page, i, horizontal);
        const int center = left + (horizontal ? page.cellWidth : page.cellHeight) / 2;
        const int distance = std::abs(coordinate - center);
        if (distance < bestDistance) { bestDistance = distance; bestIndex = i; }
    }
    return bestIndex;
}

/**
 * @brief 根据点坐标获取对应的网格单元格（页面 ID + 行列）。
 * @param point 客户区坐标。
 * @return 对应的 GridCell。
 */
inline GridCell DesktopApp::CellFromPoint(POINT point) const
{
    const GridPage* page = GridPageFromPoint(point);
    GridCell cell;
    if (!page) return cell;
    cell.pageId = page->id;
    cell.column = GetGridAxisIndexFromPoint(*page, point.x, true);
    cell.row = GetGridAxisIndexFromPoint(*page, point.y, false);
    return cell;
}

/**
 * @brief 检查网格区域是否被未选中的项目或组件占据。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 被占据返回 true。
 */
inline bool DesktopApp::IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const
{
    for (const auto& item : items_)
    {
        if (item.selected || item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (item.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = item.gridCell.column + std::max(1, item.gridSpan.columns);
        const int bottom2 = item.gridCell.row + std::max(1, item.gridSpan.rows);
        if (cell.column < right2 && right1 > item.gridCell.column &&
            cell.row < bottom2 && bottom1 > item.gridCell.row)
            return true;
    }
    for (const auto& w : widgets_)
    {
        if (w.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = w.gridCell.column + std::max(1, w.gridSpan.columns);
        const int bottom2 = w.gridCell.row + std::max(1, w.gridSpan.rows);
        if (cell.column < right2 && right1 > w.gridCell.column &&
            cell.row < bottom2 && bottom1 > w.gridCell.row)
            return true;
    }
    return false;
}

/**
 * @brief 构建选中项目的移动计划（将选中项目平移到目标单元格并保持相对位置）。
 * @param targetCell 目标单元格。
 * @return 移动计划列表，无法移动时返回空列表。
 */
inline std::vector<DesktopApp::PendingGridMove> DesktopApp::BuildSelectedMove(GridCell targetCell) const
{
    std::vector<PendingGridMove> moves;
    std::vector<size_t> selectedIndexes;
    int minColumn = INT_MAX, minRow = INT_MAX;
    int maxColumn = 0, maxRow = 0;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].selected)
        {
            selectedIndexes.push_back(i);
            minColumn = std::min(minColumn, items_[i].gridCell.column);
            minRow = std::min(minRow, items_[i].gridCell.row);
            maxColumn = std::max(maxColumn, items_[i].gridCell.column + std::max(1, items_[i].gridSpan.columns));
            maxRow = std::max(maxRow, items_[i].gridCell.row + std::max(1, items_[i].gridSpan.rows));
        }
    }
    if (selectedIndexes.empty()) return moves;

    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return moves;

    const int groupColumns = std::max(1, maxColumn - minColumn);
    const int groupRows = std::max(1, maxRow - minRow);
    const bool stacked = (groupColumns == 1 && groupRows == 1 && selectedIndexes.size() > 1);
    const int spreadCols = stacked ? std::min(static_cast<int>(selectedIndexes.size()), page->columns) : groupColumns;
    targetCell.column = std::clamp(targetCell.column, 0, std::max(0, page->columns - spreadCols));
    targetCell.row = std::clamp(targetCell.row, 0, std::max(0, page->rows - groupRows));

    int seqIndex = 0;
    std::unordered_set<std::wstring> usedSlots;
    for (size_t itemIndex : selectedIndexes)
    {
        GridCell movedCell = targetCell;
        if (stacked)
        {
            for (int attempt = 0; attempt < page->columns * page->rows; ++attempt)
            {
                int col = seqIndex / page->rows;
                int row = seqIndex % page->rows;
                GridCell probe = targetCell;
                probe.column += col;
                probe.row += row;
                ++seqIndex;
                std::wstring slotKey = probe.pageId + L":" + std::to_wstring(SlotFromCell(gridPages_, probe));
                if (probe.column <= page->columns - 1 && probe.row <= page->rows - 1 &&
                    !usedSlots.contains(slotKey) &&
                    !IsGridAreaOccupiedByUnselected(probe, items_[itemIndex].gridSpan))
                {
                    movedCell = probe;
                    usedSlots.insert(slotKey);
                    break;
                }
            }
        }
        else
        {
            movedCell.column += items_[itemIndex].gridCell.column - minColumn;
            movedCell.row += items_[itemIndex].gridCell.row - minRow;
        }

        if (!IsGridAreaValid(movedCell, items_[itemIndex].gridSpan) ||
            IsGridAreaOccupiedByUnselected(movedCell, items_[itemIndex].gridSpan))
        {
            moves.clear();
            return moves;
        }
        moves.push_back({ itemIndex, movedCell });
    }
    return moves;
}

/**
 * @brief 寻找最佳的放置单元格（优先沿拖拽方向查找空闲位置）。
 * @param targetCell 初始目标单元格。
 * @return 最佳的可用单元格。
 */
inline GridCell DesktopApp::FindBestDropCell(GridCell targetCell) const
{
    if (!BuildSelectedMove(targetCell).empty()) return targetCell;

    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return targetCell;
    const int maxCol = page->columns - 1;
    const int maxRow = page->rows - 1;

    POINT current = dragSession_.CurrentPoint();
    POINT mouseDown = dragSession_.MouseDownPoint();
    int dx = current.x - mouseDown.x;
    int dy = current.y - mouseDown.y;
    int primaryCol = 0, primaryRow = 0;
    if (std::abs(dx) >= std::abs(dy))
        primaryCol = (dx >= 0) ? 1 : -1;
    else
        primaryRow = (dy >= 0) ? 1 : -1;
    if (primaryCol == 0 && primaryRow == 0) primaryCol = 1;

    for (int dist = 1; dist <= 8; ++dist)
    {
        GridCell probe = targetCell;
        probe.column += primaryCol * dist;
        probe.row += primaryRow * dist;
        if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow) break;
        if (!BuildSelectedMove(probe).empty()) return probe;
    }

    int oppCol = -primaryCol, oppRow = -primaryRow;
    for (int dist = 1; dist <= 8; ++dist)
    {
        GridCell probe = targetCell;
        probe.column += oppCol * dist;
        probe.row += oppRow * dist;
        if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow) break;
        if (!BuildSelectedMove(probe).empty()) return probe;
    }

    for (int dist = 1; dist <= 6; ++dist)
    {
        for (int dc = -dist; dc <= dist; ++dc)
        {
            for (int dr = -dist; dr <= dist; ++dr)
            {
                if (std::abs(dc) != dist && std::abs(dr) != dist) continue;
                GridCell probe = targetCell;
                probe.column += dc;
                probe.row += dr;
                if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow) continue;
                if (!BuildSelectedMove(probe).empty()) return probe;
            }
        }
    }
    return targetCell;
}

/**
 * @brief 将选中的项目移动到目标网格单元格。
 * @param targetCell 目标单元格。
 */
inline void DesktopApp::MoveSelectedItemsToCell(GridCell targetCell)
{
    std::vector<PendingGridMove> moves = BuildSelectedMove(std::move(targetCell));
    if (moves.empty()) return;
    for (const auto& move : moves)
    {
        items_[move.index].gridCell = move.cell;
        items_[move.index].slot = SlotFromCell(gridPages_, move.cell);
    }
    LayoutItems();
    SaveLayoutSlots();
}

/**
 * @brief 更新拖拽组的原点位置（用于计算拖拽时的偏移）。
 */
inline void DesktopApp::UpdateDragGroupOrigin()
{
    int minCol = INT_MAX, minRow = INT_MAX;
    std::wstring anchorPageId;
    for (const auto& item : items_)
    {
        if (item.selected)
        {
            if (anchorPageId.empty()) anchorPageId = item.gridCell.pageId;
            minCol = std::min(minCol, item.gridCell.column);
            minRow = std::min(minRow, item.gridCell.row);
        }
    }
    GridCell groupOrigin;
    groupOrigin.pageId = anchorPageId.empty()
        ? (gridPages_.empty() ? L"" : gridPages_.front().id)
        : anchorPageId;
    groupOrigin.column = minCol != INT_MAX ? minCol : 0;
    groupOrigin.row = minRow != INT_MAX ? minRow : 0;
    RECT groupRect = GetGridRect(gridPages_, groupOrigin, GridSpan{});
    dragGroupOriginX_ = groupRect.left;
    dragGroupOriginY_ = groupRect.top;
}

/**
 * @brief 将选中的项目迁移到最后一个监视器页面。
 */
inline void DesktopApp::MigrateSelectedItemsToLastMonitorPage()
{
    if (gridPages_.empty() || lastMonitorPageId_.empty()) return;
    const GridPage* targetPage = FindGridPage(gridPages_, lastMonitorPageId_);
    if (!targetPage) return;

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& item : items_)
    {
        if (item.selected) continue;
        if (item.name.empty()) continue;
        if (item.gridCell.pageId == lastMonitorPageId_)
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    for (auto& item : items_)
    {
        if (!item.selected) continue;
        if (item.gridCell.pageId == lastMonitorPageId_) continue;

        GridCell newCell;
        newCell.pageId = lastMonitorPageId_;
        newCell.column = std::min(item.gridCell.column, std::max(0, targetPage->columns - 1));
        newCell.row = std::min(item.gridCell.row, std::max(0, targetPage->rows - 1));

        GridSpan span = item.gridSpan;
        span.columns = std::clamp(span.columns, 1, std::max(1, targetPage->columns));
        span.rows = std::clamp(span.rows, 1, std::max(1, targetPage->rows));

        if (!AreGridSlotsMarked(usedSlots, newCell, span))
        {
            MarkGridArea(usedSlots, newCell, span);
            item.gridCell = newCell;
            item.gridSpan = span;
            continue;
        }

        bool found = false;
        for (int r = 0; r < targetPage->rows && !found; ++r)
        {
            for (int c = 0; c < targetPage->columns && !found; ++c)
            {
                GridCell tryCell{ lastMonitorPageId_, c, r };
                if (!AreGridSlotsMarked(usedSlots, tryCell, span))
                {
                    MarkGridArea(usedSlots, tryCell, span);
                    item.gridCell = tryCell;
                    item.gridSpan = span;
                    found = true;
                }
            }
        }
    }
}

/**
 * @brief 获取拖拽目标点的屏幕坐标。
 * @param current 当前鼠标位置。
 * @return 拖拽目标点。
 */
inline POINT DesktopApp::GetDragTargetPoint(POINT current) const
{
    return {
        dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
        dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
    };
}

/**
 * @brief 为选中的桌面项创建 IDataObject（用于拖拽/剪贴板）。
 * @return COM 数据对象，失败返回 nullptr。
 */
inline ComPtr<IDataObject> DesktopApp::CreateSelectedDataObject() const
{
    std::vector<PCUITEMID_CHILD> pidls;
    for (const auto& item : items_)
    {
        if (item.selected)
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()));
    }
    if (pidls.empty()) return nullptr;

    ComPtr<IDataObject> dataObject;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IDataObject, nullptr,
        reinterpret_cast<void**>(dataObject.GetAddressOf()));
    if (FAILED(hr)) return nullptr;
    return dataObject;
}

/**
 * @brief 为指定文件路径列表创建文件拖拽数据对象。
 * @param paths 文件路径列表。
 * @return COM 数据对象，失败返回 nullptr。
 */
inline ComPtr<IDataObject> DesktopApp::CreateFileDropDataObject(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return nullptr;

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& path : paths)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
            pidls.push_back(pidl);
    }

    if (pidls.empty()) return nullptr;

    auto freePidls = [&]() {
        for (auto* pidl : pidls)
            ILFree(pidl);
    };

    ComPtr<IShellFolder> parentFolder;
    PCUITEMID_CHILD unusedChild = nullptr;
    HRESULT hr = SHBindToParent(pidls.front(), IID_IShellFolder,
        reinterpret_cast<void**>(parentFolder.GetAddressOf()), &unusedChild);
    if (FAILED(hr) || !parentFolder)
    {
        freePidls();
        return nullptr;
    }

    std::vector<PCUITEMID_CHILD> children;
    children.reserve(pidls.size());
    for (auto* pidl : pidls)
        children.push_back(ILFindLastID(pidl));

    ComPtr<IDataObject> dataObject;
    hr = parentFolder->GetUIObjectOf(nullptr, static_cast<UINT>(children.size()), children.data(),
        IID_IDataObject, nullptr, reinterpret_cast<void**>(dataObject.GetAddressOf()));
    freePidls();

    if (FAILED(hr)) return nullptr;
    return dataObject;
}

/**
 * @brief 根据源项目列表创建数据对象（桌面图标优先，否则用文件路径）。
 * @param sourceItems 源项目列表。
 * @return COM 数据对象，失败返回 nullptr。
 */
inline ComPtr<IDataObject> DesktopApp::CreateDataObjectForItems(
    const std::vector<Item*>& sourceItems) const
{
    DropPayload payload = DropPayload::From(sourceItems);
    if (payload.hasDesktopIcons)
    {
        if (ComPtr<IDataObject> desktopObject = CreateSelectedDataObject())
            return desktopObject;
    }
    return CreateFileDropDataObject(payload.filePaths);
}

/**
 * @brief 将选中项目拖拽放置到目标桌面项上（调用 Shell IDropTarget 接口）。
 * @param targetIndex 目标桌面项的索引。
 */
inline void DesktopApp::DropSelectedItemsOnTarget(int targetIndex)
{
    if (targetIndex < 0 || static_cast<size_t>(targetIndex) >= items_.size()) return;
    auto& targetItem = items_[targetIndex];

    ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
    if (!dataObj) return;

    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(targetItem.childPidl.get());
    ComPtr<IDropTarget> dropTarget;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, 1, &child, IID_IDropTarget, nullptr,
        reinterpret_cast<void**>(dropTarget.GetAddressOf()));
    if (FAILED(hr) || !dropTarget) return;

    POINT screenPt = dragSession_.CurrentPoint();
    ClientToScreen(hwnd_, &screenPt);
    POINTL screenPtL{ screenPt.x, screenPt.y };

    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    hr = dropTarget->DragEnter(dataObj.Get(), MK_LBUTTON, screenPtL, &effect);
    if (SUCCEEDED(hr))
        hr = dropTarget->DragOver(MK_LBUTTON, screenPtL, &effect);
    if (SUCCEEDED(hr))
        hr = dropTarget->Drop(dataObj.Get(), MK_LBUTTON, screenPtL, &effect);
    else
        dropTarget->DragLeave();
}

/**
 * @brief 根据布局键查找项目索引。
 * @param key 项目布局键。
 * @return 项目索引，未找到返回 -1。
 */
inline size_t DesktopApp::FindItemIndexByKey(const std::wstring& key) const
{
    std::wstring normalized = ToUpperInvariant(key);
    for (size_t i = 0; i < items_.size(); ++i)
        if (ToUpperInvariant(items_[i].layoutKey) == normalized) return i;
    return static_cast<size_t>(-1);
}

/**
 * @brief 从所有组件中移除指定桌面键。
 * @param keys 要移除的布局键列表。
 */
inline void DesktopApp::RemoveDesktopKeysFromWidgets(const std::vector<std::wstring>& keys)
{
    if (keys.empty()) return;

    std::vector<std::wstring> normalizedKeys;
    normalizedKeys.reserve(keys.size());
    for (const auto& key : keys)
        normalizedKeys.push_back(ToUpperInvariant(key));

    for (auto& widget : widgets_)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
            continue;
        widget.itemKeys.erase(
            std::remove_if(widget.itemKeys.begin(), widget.itemKeys.end(),
                [&](const std::wstring& existing) {
                    std::wstring normalizedExisting = ToUpperInvariant(existing);
                    return std::find(normalizedKeys.begin(), normalizedKeys.end(),
                        normalizedExisting) != normalizedKeys.end();
                }),
            widget.itemKeys.end());
    }
}

/**
 * @brief 快照当前所有桌面项的布局键。
 * @return 布局键的集合。
 */
inline std::unordered_set<std::wstring> DesktopApp::SnapshotDesktopKeys() const
{
    std::unordered_set<std::wstring> keys;
    for (const auto& item : items_)
        if (!item.layoutKey.empty())
            keys.insert(ToUpperInvariant(item.layoutKey));
    return keys;
}

/**
 * @brief 获取自快照以来新增的桌面项布局键。
 * @param existingKeys 之前的键快照。
 * @return 新增的键列表。
 */
inline std::vector<std::wstring> DesktopApp::NewDesktopKeysSince(
    const std::unordered_set<std::wstring>& existingKeys) const
{
    std::vector<std::wstring> keys;
    for (const auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!key.empty() && !existingKeys.contains(key))
            keys.push_back(key);
    }
    return keys;
}

/**
 * @brief 构建桌面放置列表，为拖拽源中的每个条目分配网格位置。
 * @param sourceList 拖拽源列表。
 * @param targetCell 目标网格单元格。
 * @param internalMove 是否为内部移动。
 * @return 放置操作列表。
 */
inline std::vector<DropLanding> DesktopApp::BuildDesktopLandings(
    const DragSourceList& sourceList, GridCell targetCell, bool internalMove) const
{
    std::vector<DropLanding> landings;
    if (sourceList.Empty()) return landings;

    if (targetCell.pageId.empty() && !gridPages_.empty())
        targetCell.pageId = gridPages_.front().id;
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return landings;

    bool desktopToDesktopMove = internalMove && sourceList.hasDesktopIcons &&
        !containers_.empty() && sourceList.origin == containers_.front().get();
    if (desktopToDesktopMove)
    {
        std::vector<PendingGridMove> moves = BuildSelectedMove(targetCell);
        if (moves.empty()) return landings;

        for (const auto& entry : sourceList.entries)
        {
            auto it = std::find_if(moves.begin(), moves.end(),
                [&](const PendingGridMove& move) { return move.index == entry.desktopIndex; });
            if (it == moves.end()) continue;
            DropLanding landing;
            landing.kind = DropLandingKind::DesktopCell;
            landing.sourceIndex = entry.sourceIndex;
            landing.cell = it->cell;
            landings.push_back(landing);
        }
        return landings;
    }

    auto advanceCell = [&](GridCell cell, GridSpan span) {
        cell.column += std::max(1, span.columns);
        if (cell.column >= page->columns)
        {
            cell.column = 0;
            cell.row += std::max(1, span.rows);
        }
        return cell;
    };

    std::unordered_set<std::wstring> sourceKeys;
    for (const auto& entry : sourceList.entries)
        if (!entry.desktopKey.empty())
            sourceKeys.insert(ToUpperInvariant(entry.desktopKey));

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (internalMove && sourceKeys.contains(ToUpperInvariant(item.layoutKey))) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    int searchSlot = SlotFromCell(gridPages_, targetCell);
    GridCell cursor = targetCell;
    for (const auto& entry : sourceList.entries)
    {
        GridSpan span = entry.originalSpan;
        span.columns = std::max(1, span.columns);
        span.rows = std::max(1, span.rows);

        GridCell cell{};
        bool found = false;
        if (IsGridAreaValid(cursor, span) && !AreGridSlotsMarked(usedSlots, cursor, span))
        {
            cell = cursor;
            found = true;
        }
        else
        {
            found = TryFindFreeCell(span, usedSlots, cell, cursor.pageId, searchSlot);
        }
        if (!found) continue;

        DropLanding landing;
        landing.kind = DropLandingKind::DesktopCell;
        landing.sourceIndex = entry.sourceIndex;
        landing.cell = cell;
        landings.push_back(landing);

        MarkGridArea(usedSlots, cell, span);
        searchSlot = SlotFromCell(gridPages_, cell) + 1;
        cursor = advanceCell(cell, span);
    }
    return landings;
}

/**
 * @brief 根据源项目和所属容器构建拖拽源列表。
 * @param sourceItems 源项目指针列表。
 * @param origin 来源容器。
 * @return 拖拽源列表。
 */
inline DragSourceList DesktopApp::BuildDragSourceList(
    const std::vector<Item*>& sourceItems, Container* origin) const
{
    DragSourceList list;
    list.origin = origin;

    WidgetContainer* originWidget = dynamic_cast<WidgetContainer*>(origin);
    DesktopWidget* originData = originWidget ? originWidget->GetWidgetData() : nullptr;
    if (originData)
    {
        list.hasOriginWidget = true;
        list.originWidgetId = originData->id;
        list.originWidgetType = originData->type;
    }

    for (auto* src : sourceItems)
    {
        if (!src) continue;
        DragSourceEntry entry;
        entry.item = src;
        entry.sourceIndex = list.entries.size();
        entry.displayName = src->GetTitle();
        entry.filePath = src->GetPath();

        if (dynamic_cast<Widget*>(src))
        {
            entry.kind = DropSourceKind::Widget;
            list.hasWidgets = true;
        }
        else if (auto* icon = dynamic_cast<DesktopIcon*>(src))
        {
            entry.kind = DropSourceKind::DesktopIcon;
            list.hasDesktopIcons = true;
            if (DesktopItem* item = icon->GetDesktopItem())
            {
                entry.desktopKey = item->layoutKey;
                entry.desktopIndex = FindItemIndexByKey(item->layoutKey);
                entry.originalCell = item->gridCell;
                entry.originalSpan = item->gridSpan;
                entry.protectedDesktopIcon = IsProtectedDesktopIcon(*item);
                if (entry.filePath.empty() && !entry.protectedDesktopIcon)
                    entry.filePath = icon->GetPath();
            }
        }
        else if (dynamic_cast<FolderEntryIcon*>(src))
        {
            entry.kind = DropSourceKind::FolderEntry;
            list.hasFolderEntries = true;
        }
        else if (dynamic_cast<ExternalFileItem*>(src))
        {
            entry.kind = DropSourceKind::ExternalFile;
            list.hasExternalFiles = true;
        }

        if (originData)
        {
            if (originData->type == DesktopWidgetType::FolderMapping)
            {
                auto it = std::find_if(originData->folderEntries.begin(), originData->folderEntries.end(),
                    [&](const FolderEntry& folderEntry) {
                        return PathsEqualInsensitive(folderEntry.fullPath, entry.filePath);
                    });
                if (it != originData->folderEntries.end())
                    entry.memberIndex = static_cast<size_t>(std::distance(originData->folderEntries.begin(), it));
            }
            else if (!entry.desktopKey.empty())
            {
                auto it = std::find_if(originData->itemKeys.begin(), originData->itemKeys.end(),
                    [&](const std::wstring& key) {
                        return ToUpperInvariant(key) == ToUpperInvariant(entry.desktopKey);
                    });
                if (it != originData->itemKeys.end())
                    entry.memberIndex = static_cast<size_t>(std::distance(originData->itemKeys.begin(), it));
            }
        }

        if (entry.originalSpan.columns <= 0) entry.originalSpan.columns = 1;
        if (entry.originalSpan.rows <= 0) entry.originalSpan.rows = 1;
        list.entries.push_back(entry);
    }
    return list;
}

/**
 * @brief 判断拖拽操作是否需要文件系统支持（复制/链接到文件夹映射等场景）。
 * @param sourceList 拖拽源列表。
 * @param targetKind 目标类型。
 * @param action 拖拽动作。
 * @return 需要文件系统支持返回 true。
 */
inline bool DesktopApp::IsDropFileBacked(const DragSourceList& sourceList,
    DropTargetKind targetKind, DropAction action) const
{
    if (sourceList.Empty()) return false;
    if (targetKind == DropTargetKind::FolderMapping) return true;
    if (action == DropAction::Copy || action == DropAction::Link) return true;
    return sourceList.hasExternalFiles || sourceList.hasFolderEntries;
}

/**
 * @brief 构建拖拽预览列表，计算放置目标、动作和落点。
 * @param sourceList 拖拽源列表。
 * @param target 目标容器。
 * @param targetSlot 目标槽位。
 * @param region 命中区域。
 * @param mods 修饰键。
 * @param dropPoint 放置点坐标。
 * @return 拖拽预览列表。
 */
inline DropPreviewList DesktopApp::BuildDropPreviewList(const DragSourceList& sourceList,
    Container* target, Slot* targetSlot, HitRegion region, int mods, POINT dropPoint) const
{
    DropPreviewList preview;
    preview.targetContainer = target;
    if (sourceList.Empty() || !target || sourceList.hasWidgets || region == HitRegion::Handoff)
        return preview;

    DropAction defaultAction = sourceList.hasExternalFiles ? DropAction::Copy : DropAction::Move;
    preview.action = DropActionFromMods(mods, defaultAction);

    if (!containers_.empty() && target == containers_.front().get())
    {
        preview.targetKind = DropTargetKind::Desktop;
        POINT adjusted = sourceList.origin ? GetDragTargetPoint(dropPoint) : dropPoint;
        GridCell targetCell = CellFromPoint(adjusted);
        bool internalMove = !IsDropFileBacked(sourceList, preview.targetKind, preview.action);
        if (internalMove)
            targetCell = FindBestDropCell(targetCell);
        preview.anchorCell = targetCell;
        preview.fileBacked = !internalMove;
        preview.landings = BuildDesktopLandings(sourceList, targetCell, internalMove);
        return preview;
    }

    if (auto* widget = dynamic_cast<WidgetContainer*>(target))
    {
        preview.targetWidget = widget->GetWidgetData();
        if (preview.targetWidget &&
            preview.targetWidget->type == DesktopWidgetType::FileCategories)
        {
            auto isShortcutPath = [](const std::wstring& path) {
                return !path.empty() && _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
            };
            bool sourceHasShortcut = std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) {
                    return isShortcutPath(entry.filePath) || isShortcutPath(entry.displayName);
                });
            if (preview.action == DropAction::Link || sourceHasShortcut)
            {
                preview.targetKind = DropTargetKind::KeyedWidget;
                return preview;
            }
        }
        preview.targetKind = preview.targetWidget &&
            preview.targetWidget->type == DesktopWidgetType::FolderMapping
                ? DropTargetKind::FolderMapping
                : DropTargetKind::KeyedWidget;
        preview.fileBacked = !(sourceList.origin == target && preview.action == DropAction::Move) &&
            IsDropFileBacked(sourceList, preview.targetKind, preview.action);
        preview.insertIndex = widget->GetDropInsertIndex(targetSlot, region);
        for (const auto& entry : sourceList.entries)
        {
            DropLanding landing;
            landing.kind = preview.targetKind == DropTargetKind::FolderMapping
                ? DropLandingKind::Folder
                : DropLandingKind::WidgetIndex;
            landing.sourceIndex = entry.sourceIndex;
            landing.widget = preview.targetWidget;
            if (preview.targetWidget)
                landing.widgetId = preview.targetWidget->id;
            landing.insertIndex = preview.insertIndex + preview.landings.size();
            if (preview.targetWidget)
                landing.cell = preview.targetWidget->gridCell;
            preview.landings.push_back(landing);
        }
        return preview;
    }

    return preview;
}

/**
 * @brief 构建外部文件拖入桌面的放置预览列表。
 * @param targetCell 目标网格单元格。
 * @param count 外部文件数量。
 * @return 拖拽预览列表。
 */
inline DropPreviewList DesktopApp::BuildExternalDesktopPreviewList(GridCell targetCell, size_t count) const
{
    DragSourceList list;
    list.hasExternalFiles = true;
    for (size_t i = 0; i < count; ++i)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = i;
        entry.originalSpan = {1, 1};
        list.entries.push_back(entry);
    }

    DropPreviewList preview;
    preview.targetKind = DropTargetKind::Desktop;
    preview.action = DropAction::Copy;
    preview.fileBacked = true;
    preview.anchorCell = targetCell;
    preview.landings = BuildDesktopLandings(list, targetCell, false);
    return preview;
}

/**
 * @brief 执行拖拽管线的完整流程（文件落地或内部移动）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
inline bool DesktopApp::ExecuteDropPipeline(const DragSourceList& sourceList,
    const DropPreviewList& preview)
{
    if (sourceList.Empty() || preview.Empty()) return false;
    return preview.fileBacked
        ? ExecuteFileBackedDropPlan(sourceList, preview)
        : ExecuteInternalDropPlan(sourceList, preview);
}

/**
 * @brief 执行内部拖拽放置计划（桌面间移动或组件间重排）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
inline bool DesktopApp::ExecuteInternalDropPlan(const DragSourceList& sourceList,
    const DropPreviewList& preview)
{
    auto sourceMemberIndices = [&]() {
        std::vector<size_t> indices;
        for (const auto& entry : sourceList.entries)
            if (entry.memberIndex != static_cast<size_t>(-1))
                indices.push_back(entry.memberIndex);
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    };

    if (preview.targetKind == DropTargetKind::Desktop)
    {
        RemoveDesktopKeysFromWidgets(sourceList.DesktopKeys());
        bool changed = false;
        for (const auto& landing : preview.landings)
        {
            auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
            if (it == sourceList.entries.end() || it->desktopIndex >= items_.size()) continue;
            items_[it->desktopIndex].gridCell = landing.cell;
            items_[it->desktopIndex].slot = SlotFromCell(gridPages_, landing.cell);
            changed = true;
        }
        if (changed)
        {
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return changed;
    }

    if (preview.targetKind == DropTargetKind::KeyedWidget && preview.targetWidget)
    {
        WidgetContainer* targetWidget = nullptr;
        for (auto& container : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (widget && widget->GetWidgetData() == preview.targetWidget)
            {
                targetWidget = widget;
                break;
            }
        }
        if (!targetWidget) return false;

        if (sourceList.origin == targetWidget && preview.action == DropAction::Move)
        {
            std::vector<size_t> indices = sourceMemberIndices();
            if (indices.empty())
                indices = targetWidget->GetSelectedMemberIndices();
            targetWidget->ReorderMembers(indices, preview.insertIndex);
            targetWidget->InvalidateSlots();
            return true;
        }

        WidgetContainer* originWidget = dynamic_cast<WidgetContainer*>(sourceList.origin);
        DesktopWidget* originData = originWidget ? originWidget->GetWidgetData() : nullptr;
        size_t inserted = 0;
        for (const auto& landing : preview.landings)
        {
            auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
            if (it == sourceList.entries.end() || it->desktopKey.empty()) continue;
            std::wstring key = ToUpperInvariant(it->desktopKey);
            if (!targetWidget->AllowsDesktopKey(key)) continue;

            if (preview.action == DropAction::Move)
            {
                if (originData)
                    RemoveDesktopKeysFromWidgets({key});
                else
                    RemoveDesktopKeysFromWidgets({key});
            }

            auto exists = std::find_if(preview.targetWidget->itemKeys.begin(),
                preview.targetWidget->itemKeys.end(),
                [&](const std::wstring& existing) { return ToUpperInvariant(existing) == key; });
            if (exists == preview.targetWidget->itemKeys.end())
            {
                size_t insertAt = std::min(preview.insertIndex + inserted, preview.targetWidget->itemKeys.size());
                preview.targetWidget->itemKeys.insert(
                    preview.targetWidget->itemKeys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
                ++inserted;
            }
            size_t itemIndex = FindItemIndexByKey(key);
            if (itemIndex != static_cast<size_t>(-1))
                items_[itemIndex].gridCell = preview.targetWidget->gridCell;
        }
        if (originWidget) originWidget->InvalidateSlots();
        targetWidget->InvalidateSlots();
        if (GetDesktopGrid()) GetDesktopGrid()->InvalidateSlots();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return true;
    }

    if (preview.targetKind == DropTargetKind::FolderMapping &&
        sourceList.origin == preview.targetContainer && preview.action == DropAction::Move)
    {
        auto* targetWidget = dynamic_cast<WidgetContainer*>(preview.targetContainer);
        if (!targetWidget) return false;
        std::vector<size_t> indices = sourceMemberIndices();
        if (indices.empty())
            indices = targetWidget->GetSelectedMemberIndices();
        targetWidget->ReorderMembers(indices, preview.insertIndex);
        return true;
    }

    return false;
}

/**
 * @brief 将文件实际复制/移动/创建快捷方式到桌面目录。
 * @param sourceList 拖拽源列表。
 * @param action 拖拽动作（复制/移动/链接）。
 * @param duplicateDesktopCopyNames 是否对已在桌面的文件生成副本名称。
 * @param createdPathsBySource 输出参数，记录每个源索引对应的创建路径。
 * @return 操作成功返回 true。
 */
inline bool DesktopApp::MaterializeFilesToDesktop(const DragSourceList& sourceList,
    DropAction action, bool duplicateDesktopCopyNames,
    std::unordered_map<size_t, std::wstring>* createdPathsBySource)
{
    std::vector<std::wstring> paths = sourceList.FilePaths();
    if (paths.empty()) return false;
    if (createdPathsBySource)
        createdPathsBySource->clear();

    wchar_t desktopPathRaw[MAX_PATH]{};
    if (!SHGetSpecialFolderPathW(nullptr, desktopPathRaw, CSIDL_DESKTOPDIRECTORY, FALSE))
        return false;
    std::wstring desktopPath = TrimTrailingPathSeparators(desktopPathRaw);

    auto doubleNull = [](const std::wstring& value) {
        std::wstring result = value;
        result.push_back(L'\0');
        result.push_back(L'\0');
        return result;
    };

    auto sameParentAsDesktop = [&](const std::wstring& path) -> bool {
        wchar_t parent[MAX_PATH]{};
        wcscpy_s(parent, path.c_str());
        if (!PathRemoveFileSpecW(parent)) return false;
        return PathsEqualInsensitive(parent, desktopPath);
    };

    auto makeUniqueCopyPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        DWORD attrs = GetFileAttributesW(path.c_str());
        bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);

        std::wstring stem = fileName ? fileName : L"";
        std::wstring ext;
        if (!isDir)
        {
            wchar_t stemBuf[MAX_PATH]{};
            wcscpy_s(stemBuf, stem.c_str());
            PathRemoveExtensionW(stemBuf);
            stem = stemBuf;
            const wchar_t* extPtr = PathFindExtensionW(fileName);
            ext = extPtr ? extPtr : L"";
        }

        for (int i = 1; i < 1000; ++i)
        {
            std::wstring name = i <= 1
                ? stem + L" - 副本" + ext
                : stem + L" - 副本 (" + std::to_wstring(i) + L")" + ext;
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES)
                return std::wstring(dst);
        }
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath.c_str(), (stem + L" - 副本 (1000)" + ext).c_str());
        return std::wstring(fallback);
    };

    auto makeUniqueShortcutPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        wchar_t stemBuf[MAX_PATH]{};
        wcscpy_s(stemBuf, fileName ? fileName : L"");
        PathRemoveExtensionW(stemBuf);
        std::wstring stem = stemBuf[0] != L'\0' ? stemBuf : L"快捷方式";

        for (int i = 1; i < 1000; ++i)
        {
            std::wstring name = i <= 1
                ? stem + L".lnk"
                : stem + L" (" + std::to_wstring(i) + L").lnk";
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES)
                return std::wstring(dst);
        }
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath.c_str(), (stem + L" (1000).lnk").c_str());
        return std::wstring(fallback);
    };

    auto shellOperateOne = [&](UINT func, const std::wstring& fromPath, const std::wstring& toPath) {
        std::wstring from = doubleNull(fromPath);
        std::wstring to = doubleNull(toPath);
        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd_;
        op.wFunc = func;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    };

    auto shellOperateManyToDesktop = [&](UINT func, const std::vector<std::wstring>& sourcePaths) {
        std::wstring from;
        for (const auto& path : sourcePaths)
        {
            from += path;
            from.push_back(L'\0');
        }
        from.push_back(L'\0');

        std::wstring to = desktopPath;
        to.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd_;
        op.wFunc = func;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    };

    bool operated = false;
    if (action == DropAction::Link)
    {
        for (const auto& source : sourceList.entries)
        {
            const auto& path = source.filePath;
            if (path.empty()) continue;

            std::wstring dst = makeUniqueShortcutPath(path);
            ComPtr<IShellLinkW> shellLink;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
            {
                shellLink->SetPath(path.c_str());
                shellLink->SetWorkingDirectory(desktopPath.c_str());
                ComPtr<IPersistFile> persistFile;
                if (SUCCEEDED(shellLink.As(&persistFile)) &&
                    SUCCEEDED(persistFile->Save(dst.c_str(), TRUE)))
                {
                    if (createdPathsBySource)
                        (*createdPathsBySource)[source.sourceIndex] = dst;
                    operated = true;
                }
            }
        }
    }
    else if (action == DropAction::Copy)
    {
        std::vector<std::wstring> normalCopies;
        for (const auto& path : paths)
        {
            if (duplicateDesktopCopyNames && sameParentAsDesktop(path))
                operated = shellOperateOne(FO_COPY, path, makeUniqueCopyPath(path)) || operated;
            else
                normalCopies.push_back(path);
        }
        if (!normalCopies.empty())
            operated = shellOperateManyToDesktop(FO_COPY, normalCopies) || operated;
    }
    else
    {
        operated = shellOperateManyToDesktop(FO_MOVE, paths);
    }
    return operated;
}

/**
 * @brief 将文件实际复制/移动/创建快捷方式到指定文件夹。
 * @param sourceList 拖拽源列表。
 * @param folderPath 目标文件夹路径。
 * @param action 拖拽动作。
 * @return 操作成功返回 true。
 */
inline bool DesktopApp::MaterializeFilesToFolder(const DragSourceList& sourceList,
    const std::wstring& folderPath, DropAction action) const
{
    std::vector<std::wstring> paths = sourceList.FilePaths();
    if (paths.empty() || folderPath.empty()) return false;

    std::wstring folder = folderPath;
    if (!folder.empty() && folder.back() != L'\\') folder += L'\\';

    if (action == DropAction::Link)
    {
        bool createdAny = false;
        for (const auto& path : paths)
        {
            std::wstring name = PathFindFileNameW(path.c_str());
            std::wstring stem = name;
            if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".lnk") == 0)
                stem = stem.substr(0, stem.size() - 4);

            std::wstring linkPath;
            for (int i = 1; i < 1000; ++i)
            {
                linkPath = folder + stem + (i == 1 ? L".lnk" : L" (" + std::to_wstring(i) + L").lnk");
                if (GetFileAttributesW(linkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                    break;
            }

            ComPtr<IShellLinkW> shellLink;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                continue;

            shellLink->SetPath(path.c_str());
            shellLink->SetWorkingDirectory(folder.c_str());
            ComPtr<IPersistFile> persistFile;
            if (SUCCEEDED(shellLink.As(&persistFile)) &&
                SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE)))
                createdAny = true;
        }
        return createdAny;
    }

    std::wstring from;
    for (const auto& path : paths)
    {
        from += path;
        from += L'\0';
    }
    from += L'\0';

    std::wstring to = folder;
    to += L'\0';
    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd_;
    op.wFunc = action == DropAction::Move ? FO_MOVE : FO_COPY;
    op.pFrom = from.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

/**
 * @brief 缓存待处理的放置结果，供后续 ApplyPendingPlacement 使用。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @param existingKeys 放置前的桌面键快照。
 * @param createdPathsBySource 可选参数，记录每个源索引对应的创建路径。
 */
inline void DesktopApp::StorePendingLandingCache(const DragSourceList& sourceList,
    const DropPreviewList& preview, const std::unordered_set<std::wstring>& existingKeys,
    const std::unordered_map<size_t, std::wstring>* createdPathsBySource)
{
    pendingLandingCache_.Clear();
    pendingLandingCache_.existingDesktopKeys = existingKeys;
    pendingLandingCache_.tick = GetTickCount();

    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell &&
            landing.kind != DropLandingKind::WidgetIndex)
            continue;
        auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
            [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
        if (it == sourceList.entries.end()) continue;

        PendingLandingEntry entry;
        entry.sourceIndex = it->sourceIndex;
        entry.action = preview.action;
        entry.kind = landing.kind;
        entry.sourcePath = it->filePath;
        entry.sourceName = !it->filePath.empty() ? FileNameFromPath(it->filePath) : it->displayName;
        if (createdPathsBySource)
        {
            auto created = createdPathsBySource->find(it->sourceIndex);
            if (created != createdPathsBySource->end())
                entry.createdPath = created->second;
        }
        entry.cell = landing.kind == DropLandingKind::DesktopCell ? landing.cell : landing.cell;
        entry.insertIndex = landing.insertIndex;
        entry.widget = landing.widget;
        entry.widgetId = landing.widgetId;
        pendingLandingCache_.entries.push_back(entry);
    }
    pendingLandingCache_.active = !pendingLandingCache_.entries.empty();
}

/**
 * @brief 执行基于文件系统的拖拽放置计划（复制/移动到桌面或文件夹映射）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
inline bool DesktopApp::ExecuteFileBackedDropPlan(const DragSourceList& sourceList,
    const DropPreviewList& preview)
{
    auto refreshFolderMappingById = [&](const std::wstring& widgetId) {
        if (widgetId.empty()) return;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (widgets_[i].id == widgetId)
            {
                RefreshFolderMappingWidget(i);
                break;
            }
        }
    };
    auto removeDesktopItemsByKeys = [&](const std::vector<std::wstring>& keys) {
        if (keys.empty()) return false;

        std::unordered_set<std::wstring> normalizedKeys;
        normalizedKeys.reserve(keys.size());
        for (const auto& key : keys)
            if (!key.empty())
                normalizedKeys.insert(ToUpperInvariant(key));
        if (normalizedKeys.empty()) return false;

        size_t oldSize = items_.size();
        items_.erase(
            std::remove_if(items_.begin(), items_.end(),
                [&](const DesktopItem& item) {
                    return !item.layoutKey.empty() &&
                        normalizedKeys.contains(ToUpperInvariant(item.layoutKey));
                }),
            items_.end());
        if (items_.size() == oldSize) return false;

        d2dIconCache_.clear();
        if (GetDesktopGrid())
            GetDesktopGrid()->InvalidateSlots();
        return true;
    };

    if (preview.targetKind == DropTargetKind::FolderMapping && preview.targetWidget)
    {
        size_t targetWidgetIndex = static_cast<size_t>(-1);
        std::unordered_set<std::wstring> targetExistingPaths;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (&widgets_[i] != preview.targetWidget) continue;
            targetWidgetIndex = i;
            for (const auto& entry : widgets_[i].folderEntries)
                targetExistingPaths.insert(ToUpperInvariant(entry.fullPath));
            break;
        }

        bool operated = MaterializeFilesToFolder(sourceList, preview.targetWidget->sourceFolderPath,
            preview.action);
        if (!operated) return false;
        if (preview.action == DropAction::Move)
        {
            RemoveDesktopKeysFromWidgets(sourceList.DesktopKeys());
            removeDesktopItemsByKeys(sourceList.DesktopKeys());
        }

        if (sourceList.hasOriginWidget &&
            sourceList.originWidgetType == DesktopWidgetType::FolderMapping)
            refreshFolderMappingById(sourceList.originWidgetId);
        if (targetWidgetIndex != static_cast<size_t>(-1))
        {
            RefreshFolderMappingWidget(targetWidgetIndex);
            auto& target = widgets_[targetWidgetIndex];
            std::vector<FolderEntry> inserted;
            for (auto it = target.folderEntries.begin(); it != target.folderEntries.end(); )
            {
                if (targetExistingPaths.contains(ToUpperInvariant(it->fullPath)))
                {
                    ++it;
                    continue;
                }
                inserted.push_back(std::move(*it));
                it = target.folderEntries.erase(it);
            }
            if (!inserted.empty())
            {
                size_t insertAt = std::min(preview.insertIndex, target.folderEntries.size());
                target.folderEntries.insert(target.folderEntries.begin() + static_cast<std::ptrdiff_t>(insertAt),
                    std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));
                target.itemKeys.clear();
                target.itemKeys.reserve(target.folderEntries.size());
                for (const auto& entry : target.folderEntries)
                    target.itemKeys.push_back(entry.fullPath);
            }
            for (auto& c : containers_)
            {
                auto* wc = dynamic_cast<WidgetContainer*>(c.get());
                if (wc && wc->GetWidgetData() == &target) { wc->InvalidateSlots(); break; }
            }
        }
        return true;
    }

    bool duplicateCopyNames = preview.action == DropAction::Copy && sourceList.hasDesktopIcons &&
        !sourceList.hasExternalFiles;
    std::unordered_set<std::wstring> existingKeys = SnapshotDesktopKeys();
    std::unordered_map<size_t, std::wstring> createdPathsBySource;
    std::unordered_map<size_t, std::wstring>* createdPaths =
        preview.action == DropAction::Link ? &createdPathsBySource : nullptr;
    bool operated = MaterializeFilesToDesktop(sourceList, preview.action, duplicateCopyNames,
        createdPaths);
    if (!operated)
    {
        pendingLandingCache_.Clear();
        return false;
    }
    StorePendingLandingCache(sourceList, preview, existingKeys, createdPaths);

    ReloadItems(false);
    if (preview.action == DropAction::Move && sourceList.hasDesktopIcons)
        RemoveDesktopKeysFromWidgets(sourceList.DesktopKeys());
    if (sourceList.hasOriginWidget &&
        sourceList.originWidgetType == DesktopWidgetType::FolderMapping)
        refreshFolderMappingById(sourceList.originWidgetId);
    return true;
}

/**
 * @brief 在 D2D 设备上下文上绘制拖拽放置预览（高亮目标区域）。
 * @param ctx D2D 设备上下文。
 * @param preview 拖拽预览列表。
 */
inline void DesktopApp::DrawDesktopDropPreviewList(ID2D1DeviceContext* ctx,
    const DropPreviewList& preview)
{
    if (!ctx) return;
    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell) continue;
        GridSpan span{1, 1};
        auto it = std::find_if(items_.begin(), items_.end(), [&](const DesktopItem& item) {
            return item.gridCell.pageId == landing.cell.pageId &&
                item.gridCell.column == landing.cell.column &&
                item.gridCell.row == landing.cell.row;
        });
        if (it != items_.end())
            span = it->gridSpan;
        RECT targetRect = GetGridRect(gridPages_, landing.cell, span);
        DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
    }
}

/**
 * @brief 应用缓存的放置结果，将新创建的文件分配到正确的网格位置或组件中。
 */
inline void DesktopApp::ApplyPendingPlacement()
{
    if (!pendingLandingCache_.active) return;
    if (GetTickCount() - pendingLandingCache_.tick > 10000)
    {
        pendingLandingCache_.Clear();
        return;
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!key.empty() && !pendingLandingCache_.existingDesktopKeys.contains(key))
            continue;
        if (!item.name.empty() && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    auto findWidgetContainer = [&](const std::wstring& widgetId) -> WidgetContainer* {
        for (auto& container : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
            if (data && data->id == widgetId)
                return widget;
        }
        return nullptr;
    };

    std::vector<bool> entryUsed(pendingLandingCache_.entries.size(), false);
    bool changed = false;
    for (size_t itemIndex = 0; itemIndex < items_.size(); ++itemIndex)
    {
        auto& item = items_[itemIndex];
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (key.empty() || pendingLandingCache_.existingDesktopKeys.contains(key))
            continue;

        for (size_t e = 0; e < pendingLandingCache_.entries.size(); ++e)
        {
            if (entryUsed[e]) continue;
            const auto& landing = pendingLandingCache_.entries[e];
            bool matchesLanding = false;
            if (!landing.createdPath.empty())
            {
                matchesLanding =
                    PathsEqualInsensitive(item.parsingName, landing.createdPath) ||
                    PathsEqualInsensitive(FileNameFromPath(item.parsingName),
                        FileNameFromPath(landing.createdPath)) ||
                    PathsEqualInsensitive(item.name, FileNameFromPath(landing.createdPath));
            }
            if (!matchesLanding)
            {
                matchesLanding =
                    MatchPendingName(item.name, landing.sourceName) ||
                    (!item.parsingName.empty() &&
                     MatchPendingName(FileNameFromPath(item.parsingName), landing.sourceName));
            }
            if (!matchesLanding) continue;

            if (landing.kind == DropLandingKind::WidgetIndex && !landing.widgetId.empty())
            {
                WidgetContainer* widget = findWidgetContainer(landing.widgetId);
                DesktopWidget* widgetData = widget ? widget->GetWidgetData() : nullptr;
                if (!widgetData) break;

                item.gridCell = widgetData->gridCell;
                bool allowKey = !widget || landing.action == DropAction::Link || widget->AllowsDesktopKey(key);
                if (allowKey)
                {
                    auto exists = std::find_if(widgetData->itemKeys.begin(), widgetData->itemKeys.end(),
                        [&](const std::wstring& existing) { return ToUpperInvariant(existing) == key; });
                    if (exists == widgetData->itemKeys.end())
                    {
                        size_t insertAt = std::min(landing.insertIndex, widgetData->itemKeys.size());
                        widgetData->itemKeys.insert(
                            widgetData->itemKeys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
                    }
                    if (widget) widget->InvalidateSlots();
                }
            }
            else if (landing.kind == DropLandingKind::DesktopCell)
            {
                GridSpan span = item.gridSpan;
                span.columns = std::max(1, span.columns);
                span.rows = std::max(1, span.rows);

                GridCell cell = landing.cell;
                bool found = false;
                if (IsGridAreaValid(cell, span) && !AreGridSlotsMarked(usedSlots, cell, span))
                {
                    found = true;
                }
                else
                {
                    found = TryFindFreeCell(span, usedSlots, cell, landing.cell.pageId,
                        SlotFromCell(gridPages_, landing.cell));
                }
                if (!found) break;
                item.gridCell = cell;
                item.slot = SlotFromCell(gridPages_, cell);
                item.selected = true;
                MarkGridArea(usedSlots, cell, span);
            }

            entryUsed[e] = true;
            changed = true;
            break;
        }
    }

    std::vector<PendingLandingEntry> remaining;
    for (size_t i = 0; i < pendingLandingCache_.entries.size(); ++i)
        if (!entryUsed[i])
            remaining.push_back(pendingLandingCache_.entries[i]);

    pendingLandingCache_.entries = std::move(remaining);
    pendingLandingCache_.active = !pendingLandingCache_.entries.empty();
    if (!pendingLandingCache_.active)
        pendingLandingCache_.existingDesktopKeys.clear();

    if (changed)
    {
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

// ── 网格全局函数 ──────────────────────────────────────────

/**
 * @brief 根据页面 ID 在页面列表中查找对应的网格页面。
 * @param pages 页面列表。
 * @param pageId 页面 ID。
 * @return 找到的页面指针，未找到返回 nullptr。
 */
inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId)
{
    for (auto& p : pages) if (p.id == pageId) return &p;
    return nullptr;
}

/**
 * @brief 获取网格轴上指定索引的偏移像素值。
 * @param page 网格页面。
 * @param index 索引。
 * @param horizontal true 为水平轴，false 为垂直轴。
 * @return 偏移像素值。
 */
inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal)
{
    const int count = horizontal ? page.columns : page.rows;
    const int cellSize = horizontal ? page.cellWidth : page.cellHeight;
    if (index <= 0 || count <= 1) return std::max(0, index) * cellSize;

    const int extent = horizontal
        ? static_cast<int>(page.workArea.right - page.workArea.left)
        : static_cast<int>(page.workArea.bottom - page.workArea.top);
    const int margin = horizontal ? page.marginX : page.marginY;
    const int gapSpace = std::max(0, extent - margin * 2 - count * cellSize);
    const int gapCount = count - 1;

    // Use a cumulative ratio so integer remainders are spread across all
    // internal gaps instead of being absorbed by the two outer margins.
    const int distributedGap = (index * gapSpace + gapCount / 2) / gapCount;
    return index * cellSize + distributedGap;
}

/**
 * @brief 根据网格页面和单元格计算对应的矩形区域。
 * @param pages 页面列表。
 * @param cell 起始单元格。
 * @param span 跨度（默认 {1,1}）。
 * @return 计算出的 RECT。
 */
inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span = {})
{
    auto* page = FindGridPage(pages, cell.pageId);
    if (!page) return MakeRect(0, 0, 0, 0);
    int col = std::clamp(cell.column, 0, std::max(0, page->columns - 1));
    int row = std::clamp(cell.row,    0, std::max(0, page->rows    - 1));
    int sc  = std::clamp(span.columns, 1, std::max(1, page->columns - col));
    int sr  = std::clamp(span.rows,    1, std::max(1, page->rows    - row));
    int x = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col, true);
    int y = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row, false);
    int r = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col + sc - 1, true)  + page->cellWidth;
    int b = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row + sr - 1, false) + page->cellHeight;
    return MakeRect(x, y, r, b);
}

/**
 * @brief 根据网格页面和单元格计算槽位索引。
 * @param pages 页面列表。
 * @param cell 单元格。
 * @return 槽位索引。
 */
inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell)
{
    auto* page = FindGridPage(pages, cell.pageId);
    int rows = page ? page->rows : 1;
    return std::max(0, cell.column) * std::max(1, rows) + std::max(0, cell.row);
}

/**
 * @brief 对所有桌面项和组件执行网格布局计算，更新每个项的边界矩形和槽位。
 */
inline void DesktopApp::LayoutItems()
{
    for (auto& item : items_)
    {
        if (item.name.empty()) { item.bounds = {}; continue; }
        if (!gridPages_.empty() && item.gridCell.pageId.empty())
            item.gridCell.pageId = gridPages_.front().id;
        auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (page)
        {
            item.gridSpan.columns = std::clamp(item.gridSpan.columns, 1, std::max(1, page->columns));
            item.gridSpan.rows    = std::clamp(item.gridSpan.rows,    1, std::max(1, page->rows));
            item.gridCell.column  = std::clamp(item.gridCell.column,  0, std::max(0, page->columns - item.gridSpan.columns));
            item.gridCell.row     = std::clamp(item.gridCell.row,     0, std::max(0, page->rows    - item.gridSpan.rows));
        }
        item.slot   = SlotFromCell(gridPages_, item.gridCell);
        item.bounds = GetGridRect(gridPages_, item.gridCell, item.gridSpan);
    }

    // Layout widgets
    for (auto& widget : widgets_)
    {
        if (!gridPages_.empty() && widget.gridCell.pageId.empty())
            widget.gridCell.pageId = gridPages_.front().id;
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (page)
        {
            widget.gridSpan = ClampWidgetGridSpan(widget, widget.gridSpan,
                page->columns, page->rows);
            widget.gridCell.column  = std::clamp(widget.gridCell.column,  0, std::max(0, page->columns - widget.gridSpan.columns));
            widget.gridCell.row     = std::clamp(widget.gridCell.row,     0, std::max(0, page->rows    - widget.gridSpan.rows));
        }
        widget.bounds = GetGridRect(gridPages_, widget.gridCell, widget.gridSpan);
    }

    RebuildContainersAndItems();
}

/**
 * @brief 重建容器和项目对象层次结构（DesktopGrid、DesktopIcon、WidgetContainer 等）。
 *
 * 根据 items_ 和 widgets_ 数据重建运行时的 Container 和 Item 对象，
 * 并重新绑定拖拽源。
 */
inline void DesktopApp::RebuildContainersAndItems()
{
    containers_.clear();
    items_oo_.clear();

    // Collect keys of items that belong to widgets
    std::unordered_set<std::wstring> collectedKeys;
    collectedKeysCache_.clear();
    for (auto& w : widgets_)
        for (auto& k : w.itemKeys)
        {
            auto upper = ToUpperInvariant(k);
            collectedKeys.insert(upper);
            collectedKeysCache_.insert(upper);
        }

    // DesktopGrid
    auto grid = std::make_unique<DesktopGrid>(&gridPages_, &items_, this);
    containers_.push_back(std::move(grid));

    // DesktopIcon for each NON-collected DesktopItem
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (collectedKeys.contains(ToUpperInvariant(item.layoutKey))) continue;
        auto icon = std::make_unique<DesktopIcon>(&item, containers_.back().get(), this);
        items_oo_.push_back(std::move(icon));
    }

    // Widgets
    for (auto& w : widgets_)
    {
        auto widget = CreateWidget(&w, this);
        if (!widget) continue;

        if (auto* wc = dynamic_cast<WidgetContainer*>(widget.get()))
        {
            widget.release();
            containers_.push_back(std::unique_ptr<Container>(wc));
        }
        else
        {
            items_oo_.push_back(std::move(widget));
        }
    }
    RebindDragSourceAfterRebuild();
    InvalidateDragStaticScene();
}

/**
 * @brief 枚举文件夹映射组件对应的物理目录，填充 folderEntries 列表。
 * @param widget 目标组件。
 */
inline void DesktopApp::EnumerateFolderMappingEntries(DesktopWidget& widget)
{
    for (auto& entry : widget.folderEntries)
        if (entry.iconBitmap) DeleteObject(entry.iconBitmap);
    widget.folderEntries.clear();
    if (widget.sourceFolderPath.empty()) return;
    std::wstring search = widget.sourceFolderPath + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        FolderEntry entry;
        entry.name = fd.cFileName;
        entry.fullPath = widget.sourceFolderPath + L"\\" + fd.cFileName;
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        SHFILEINFOW info{};
        SHGetFileInfoW(entry.fullPath.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX);
        entry.sysIconIndex = info.iIcon;

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
        {
            entry.iconBitmap = GetHighResolutionShellIconBitmap(pidl, info.iIcon, entry.iconBitmapSize);
            ILFree(pidl);
        }
        if (entry.iconBitmap)
        {
            ClampAlphaToColorKey(entry.iconBitmap, kTransparentKey);
            std::wstring upper = ToUpperInvariant(entry.name);
            if (upper.size() > 4 && upper.compare(upper.size() - 4, 4, L".LNK") == 0)
                ApplyShortcutArrowToBitmap(entry.iconBitmap, entry.iconBitmapSize);
        }
        widget.folderEntries.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    std::sort(widget.folderEntries.begin(), widget.folderEntries.end(),
        [](const FolderEntry& a, const FolderEntry& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    if (!widget.itemKeys.empty())
    {
        std::unordered_map<std::wstring, size_t> order;
        for (size_t i = 0; i < widget.itemKeys.size(); ++i)
            order[ToUpperInvariant(widget.itemKeys[i])] = i;
        std::stable_sort(widget.folderEntries.begin(), widget.folderEntries.end(),
            [&](const FolderEntry& a, const FolderEntry& b) {
                auto ia = order.find(ToUpperInvariant(a.fullPath));
                auto ib = order.find(ToUpperInvariant(b.fullPath));
                bool ha = ia != order.end();
                bool hb = ib != order.end();
                if (ha != hb) return ha;
                if (ha && hb) return ia->second < ib->second;
                return false;
            });
    }
    widget.itemKeys.clear();
    widget.itemKeys.reserve(widget.folderEntries.size());
    for (const auto& entry : widget.folderEntries)
        widget.itemKeys.push_back(entry.fullPath);
}

/**
 * @brief 刷新文件夹映射组件的内容（重新枚举目录）。
 * @param widgetIndex 组件索引。
 */
inline void DesktopApp::RefreshFolderMappingWidget(size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return;
    auto& w = widgets_[widgetIndex];
    for (auto& e : w.folderEntries)
        if (e.iconBitmap) DeleteObject(e.iconBitmap);
    w.folderEntries.clear();
    EnumerateFolderMappingEntries(w);
    for (auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (wc && wc->GetWidgetData() == &w) { wc->InvalidateSlots(); break; }
    }
    // NOTE: caller must RebuildContainersAndItems + SaveLayoutSlots + InvalidateDesktop
}

/**
 * @brief 收集桌面文件到文件分类组件中。
 * @param widgetIndex 组件索引。
 * @param persist 是否立即持久化布局。
 * @return 有变化返回 true。
 */
inline bool DesktopApp::CollectFileCategoryWidget(size_t widgetIndex, bool persist)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FileCategories)
        return false;

    FileCategories collector(&widgets_[widgetIndex], this);
    bool changed = collector.CollectTopLevelDesktopItems();
    if (!changed) return false;

    if (persist)
    {
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    return true;
}

/**
 * @brief 确保只有一个文件分类组件开启自动收集模式。
 * @param activeWidgetIndex 当前激活的组件索引。
 */
inline void DesktopApp::EnforceSingleAutoCollectFileCategory(size_t activeWidgetIndex)
{
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i != activeWidgetIndex && widgets_[i].type == DesktopWidgetType::FileCategories)
            widgets_[i].autoCollect = false;
    }
}

/**
 * @brief 应用所有开启了 autoCollect 的文件分类组件的自动收集。
 */
inline void DesktopApp::ApplyAutoCollectFileCategoryWidgets()
{
    size_t active = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].type == DesktopWidgetType::FileCategories && widgets_[i].autoCollect)
        {
            if (active == static_cast<size_t>(-1))
                active = i;
            else
                widgets_[i].autoCollect = false;
        }
    }
    if (active != static_cast<size_t>(-1))
        CollectFileCategoryWidget(active, false);
}

// ── 组件创建辅助函数 ──────────────────────────────────

/**
 * @brief 生成一个新的唯一组件 ID。
 * @return 组件 ID 字符串。
 */
inline std::wstring DesktopApp::MakeNewWidgetId() const
{
    return L"widget-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
}

inline void DesktopApp::ConfigureWidgetGridLimits(DesktopWidget& widget) const
{
    widget.minGridSpan = { 1, 1 };
    widget.maxGridSpan = { 0, 0 };

    if (widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping)
    {
        widget.minGridSpan = { 2, 2 };
    }
    else if (widget.type == DesktopWidgetType::LuaScript && !widget.scriptPath.empty())
    {
        LuaWidgetManifest manifest = WidgetEngine::GetWidgetManifest(widget.scriptPath);
        widget.minGridSpan = {
            std::max(1, manifest.minColumns),
            std::max(1, manifest.minRows)
        };
        widget.maxGridSpan = {
            std::max(0, manifest.maxColumns),
            std::max(0, manifest.maxRows)
        };
    }
}

inline GridSpan DesktopApp::ClampWidgetGridSpan(const DesktopWidget& widget, GridSpan span,
    int availableColumns, int availableRows) const
{
    const int pageMaxColumns = std::max(1, availableColumns);
    const int pageMaxRows = std::max(1, availableRows);
    const int maxColumns = widget.maxGridSpan.columns > 0
        ? std::min(pageMaxColumns, widget.maxGridSpan.columns)
        : pageMaxColumns;
    const int maxRows = widget.maxGridSpan.rows > 0
        ? std::min(pageMaxRows, widget.maxGridSpan.rows)
        : pageMaxRows;
    const int minColumns = std::min(maxColumns, std::max(1, widget.minGridSpan.columns));
    const int minRows = std::min(maxRows, std::max(1, widget.minGridSpan.rows));

    span.columns = std::clamp(span.columns, minColumns, maxColumns);
    span.rows = std::clamp(span.rows, minRows, maxRows);
    return span;
}

/**
 * @brief 将组件添加到网格中，自动查找空闲位置。
 * @param widget 组件对象（移动语义）。
 * @param span 组件跨度。
 */
inline void DesktopApp::AddWidgetToGrid(DesktopWidget&& widget, GridSpan span)
{
    ConfigureWidgetGridLimits(widget);
    POINT clientPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &clientPoint);
    GridCell cell = CellFromPoint(clientPoint);
    if (cell.pageId.empty())
    {
        for (const auto& p : gridPages_) { cell = { p.id, 0, 0 }; break; }
        if (cell.pageId.empty()) return;
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell freeCell;
    const GridPage* cellPage = FindGridPage(gridPages_, cell.pageId);
    if (cellPage)
        span = ClampWidgetGridSpan(widget, span, cellPage->columns, cellPage->rows);
    bool needSearch = AreGridSlotsMarked(usedSlots, cell, span) ||
        !IsGridAreaValid(cell, span);
    if (!needSearch && cellPage)
    {
        if (cell.column + span.columns > cellPage->columns ||
            cell.row + span.rows > cellPage->rows)
            needSearch = true;
    }
    if (needSearch)
    {
        int startSlot = 0;
        const GridPage* page = FindGridPage(gridPages_, cell.pageId);
        if (page)
            startSlot = cell.column * std::max(1, page->rows) + cell.row;
        if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, startSlot))
        {
            if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, 0))
                TryFindFreeCell(span, usedSlots, freeCell, L"", 0);
        }
        if (freeCell.pageId.empty()) return;
        cell = freeCell;
    }

    widget.gridCell = cell;
    widget.gridSpan = span;
    widgets_.push_back(std::move(widget));
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 在右键菜单位置添加集合组件。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::AddCollectionWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::Collection;
    w.title = L"集合";
    w.showTitle = true;
    w.bottomBarHover = true;
    AddWidgetToGrid(std::move(w), { 1, 1 });
}

/**
 * @brief 在右键菜单位置添加桌面文件分类组件。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::AddFileCategoryWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::FileCategories;
    w.title = L"桌面文件";
    w.showTitle = true;
    AddWidgetToGrid(std::move(w), { 2, 2 });
}

/**
 * @brief 在右键菜单位置添加文件夹映射组件（弹出文件夹选择对话框）。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::AddFolderMappingWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;

    // Pick source folder
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"选择要映射的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH]{};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    std::wstring folderPath(path);
    std::wstring title = folderPath;
    if (!title.empty() && title.back() == L'\\') title.pop_back();
    size_t lastSep = title.find_last_of(L"\\/");
    title = (lastSep != std::wstring::npos) ? title.substr(lastSep + 1) : title;

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::FolderMapping;
    w.title = title;
    w.showTitle = true;
    w.sourceFolderPath = folderPath;
    AddWidgetToGrid(std::move(w), { 2, 2 });

    // Enumerate entries
    size_t idx = widgets_.size() - 1;
    EnumerateFolderMappingEntries(widgets_[idx]);
    RebuildContainersAndItems();
}

/**
 * @brief 在右键菜单位置添加 Lua 脚本组件。
 * @param screenPoint 屏幕坐标点。
 * @param scriptFilename 脚本文件名。
 */
inline void DesktopApp::AddLuaWidgetAt(POINT screenPoint, const std::wstring& scriptFilename)
{
    if (scriptFilename.empty()) return;
    lastContextMenuScreenPoint_ = screenPoint;

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::LuaScript;
    w.title = WidgetEngine::GetWidgetDisplayName(scriptFilename);
    if (w.title.empty())
    {
        w.title = scriptFilename;
        if (w.title.size() > 4 && ToUpperInvariant(w.title.substr(w.title.size() - 4)) == L".LUA")
            w.title.resize(w.title.size() - 4);
    }
    w.scriptPath = scriptFilename;
    w.bottomBarHover = true;
    if (widgetEngine_)
    {
        widgetEngine_->EnsureWidgetLoaded(w.id, scriptFilename);
        w.showTitle = widgetEngine_->ReadBoolFlag(scriptFilename, "showTitle", false);
        w.bottomBarHover = widgetEngine_->ReadBoolFlag(scriptFilename, "bottomBarHover", true);
    }
    int defaultColumns = 1;
    int defaultRows = 1;
    WidgetEngine::GetWidgetDefaultSpan(scriptFilename, defaultColumns, defaultRows);
    AddWidgetToGrid(std::move(w), { defaultColumns, defaultRows });
}

/**
 * @brief 将组件放置到指定网格位置，并重新安置被挤占的桌面项。
 * @param widgetIndex 组件索引。
 * @param targetCell 目标单元格。
 * @param targetSpan 目标跨度。
 * @param isMove 是否为移动操作（而非缩放）。
 */
inline void DesktopApp::PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan, bool isMove)
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

    if (isMove)
    {
        // For move: displaced items go to the widget's old position area
        // Free the old area in usedSlots so items can be placed there
        // (it's already excluded since we skipped widgetIndex)
        // Sort displaced by grid order
        auto byGrid = [this](size_t a, size_t b) {
            if (items_[a].gridCell.pageId != items_[b].gridCell.pageId)
                return items_[a].gridCell.pageId < items_[b].gridCell.pageId;
            int sa = SlotFromCell(gridPages_, items_[a].gridCell);
            int sb = SlotFromCell(gridPages_, items_[b].gridCell);
            return sa < sb;
        };
        std::sort(displaced.begin(), displaced.end(), byGrid);

        // Apply widget placement
        widgets_[widgetIndex].gridCell = targetCell;
        widgets_[widgetIndex].gridSpan = targetSpan;

        // Place displaced items into the old widget area first, then elsewhere
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
        }
    }
    else
    {
        // For resize: push displaced items to new free cells (original behavior)
        // Sort displaced by grid order for deterministic placement
        auto byGrid = [this](size_t a, size_t b) {
            if (items_[a].gridCell.pageId != items_[b].gridCell.pageId)
                return items_[a].gridCell.pageId < items_[b].gridCell.pageId;
            int sa = SlotFromCell(gridPages_, items_[a].gridCell);
            int sb = SlotFromCell(gridPages_, items_[b].gridCell);
            return sa < sb;
        };
        std::sort(displaced.begin(), displaced.end(), byGrid);

        // Apply widget placement
        widgets_[widgetIndex].gridCell = targetCell;
        widgets_[widgetIndex].gridSpan = targetSpan;

        // Re-home displaced items starting after target
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
        }
    }

    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
}

/**
 * @brief 获取或创建 HBITMAP 对应的 Direct2D 位图（带缓存）。
 * @param hbm HBITMAP 句柄。
 * @return ID2D1Bitmap1 指针，失败返回 nullptr。
 */
inline ID2D1Bitmap1* DesktopApp::GetOrCreateD2DBitmap(HBITMAP hbm)
{
    if (!hbm) return nullptr;
    auto key = reinterpret_cast<std::uintptr_t>(hbm);
    auto it = d2dIconCache_.find(key);
    if (it != d2dIconCache_.end()) return it->second.Get();

    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm)) return nullptr;

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(d2dContext_->CreateBitmap(D2D1::SizeU(bm.bmWidth, bm.bmHeight),
        nullptr, 0, &props, &bitmap)))
        return nullptr;

    D2D1_RECT_U dst = D2D1::RectU(0, 0, static_cast<UINT32>(bm.bmWidth), static_cast<UINT32>(bm.bmHeight));
    bitmap->CopyFromMemory(&dst, bm.bmBits, bm.bmWidthBytes);

    auto* result = bitmap.Get();
    d2dIconCache_[key] = bitmap;
    return result;
}
