#include "app.h"
#include "../desktop_drop_search.h"
#include "../widgets/collection_group_rules.h"
#include "../windows_desktop_layout.h"
#include "../windows_desktop_layout_rules.h"

// Grid geometry, drag-group planning and cross-monitor migration.

void DesktopApp::InitializeGridFromWindows()
{
    if (!initializeGridFromWindows_) return;
    initializeGridFromWindows_ = false;
    if (gridPages_.empty() || !layoutRecords_.empty() ||
        !widgets_.empty() || !dockEntries_.empty()) return;

    namespace native = snowdesktop::windows_desktop_layout;
    const auto snapshot = native::Capture();
    if (!snapshot.Available())
    {
        const std::wstring message = L"Windows desktop grid capture unavailable; using DPI defaults. HRESULT=" +
            std::to_wstring(static_cast<unsigned long>(snapshot.status));
        WriteDiagnosticLogEntry(message.c_str());
        return;
    }

    std::unordered_map<std::wstring, POINT> positions;
    for (const auto& item : snapshot.items)
        positions.emplace(ToUpperInvariant(item.parsingName), item.screenPosition);
    std::vector<POINT> spacingByPage;
    spacingByPage.reserve(gridPages_.size());
    for (auto& page : gridPages_)
    {
        const POINT spacing{
            native::MonitorSpacing(snapshot.spacing.x, snapshot.spacingDpi, page.dpiX),
            native::MonitorSpacing(snapshot.spacing.y, snapshot.spacingDpi, page.dpiY)};
        spacingByPage.push_back(spacing);
        page.columns = native::AxisCount(
            page.workArea.right - page.workArea.left, spacing.x);
        page.rows = native::AxisCount(
            page.workArea.bottom - page.workArea.top, spacing.y);
        page.columns = std::max(1, page.columns);
        page.rows = std::max(1, page.rows);
    }

    struct Placement
    {
        size_t itemIndex;
        size_t pageIndex;
        native::Cell requested;
    };
    std::vector<Placement> placements;
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const auto& item = items_[index];
        if (item.name.empty()) continue;
        const auto found = positions.find(ToUpperInvariant(item.parsingName));
        if (found == positions.end()) continue;
        // GridPage bounds and work areas are relative to the virtual desktop,
        // while the Shell capture uses physical screen coordinates.
        const POINT point{found->second.x - virtualLeft_, found->second.y - virtualTop_};
        for (size_t pageIndex = 0; pageIndex < gridPages_.size(); ++pageIndex)
        {
            auto& page = gridPages_[pageIndex];
            if (!PtInRect(&page.bounds, point)) continue;
            const POINT spacing = spacingByPage[pageIndex];
            const native::Cell cell{
                native::AxisIndex(point.x, page.workArea.left, spacing.x),
                native::AxisIndex(point.y, page.workArea.top, spacing.y)};
            // Explorer may use a partially visible last row/column. Preserve
            // those cells instead of folding their items onto the previous one.
            page.columns = std::max(page.columns, cell.column + 1);
            page.rows = std::max(page.rows, cell.row + 1);
            placements.push_back({index, pageIndex, cell});
            break;
        }
    }

    std::vector<std::vector<bool>> occupied;
    occupied.reserve(gridPages_.size());
    for (auto& page : gridPages_)
    {
        savedPageColumns_[page.id] = page.columns;
        savedPageRows_[page.id] = page.rows;
        ApplyIconSpacingToPage(page);
        occupied.emplace_back(static_cast<size_t>(page.columns * page.rows), false);
    }
    size_t imported = 0;
    for (const auto& placement : placements)
    {
        const auto& page = gridPages_[placement.pageIndex];
        const auto cell = native::ClaimNearestCell(placement.requested,
            page.columns, page.rows, occupied[placement.pageIndex]);
        if (!cell) continue; // Existing overflow placement handles a full page.
        auto& item = items_[placement.itemIndex];
        item.gridCell = {page.id, cell->column, cell->row};
        item.gridSpan = {1, 1};
        ++imported;
    }
    RefreshIconBitmapResolution();
    const std::wstring message = L"Initialized grid from Windows: spacing=" +
        std::to_wstring(snapshot.spacing.x) + L"x" + std::to_wstring(snapshot.spacing.y) +
        L", icon=" + std::to_wstring(snapshot.iconSize) +
        L", captured=" + std::to_wstring(snapshot.items.size()) +
        L", imported=" + std::to_wstring(imported);
    WriteDiagnosticLogEntry(message.c_str());
}

void DesktopApp::UpdateLayoutWorkArea(bool preserveActiveDimensions)
{
    layoutWorkArea_ = MakeRect(0, 0, virtualWidth_, virtualHeight_);
    // Preserve the active page dimensions before rebuilding monitor geometry.
    // DPI, resolution and work-area changes may resize cells, but must not
    // replace an already established row/column count.
    if (preserveActiveDimensions)
    {
        for (const auto& page : gridPages_)
        {
            if (page.id.empty()) continue;
            savedPageColumns_[page.id] = std::max(1, page.columns);
            savedPageRows_[page.id] = std::max(1, page.rows);
        }
    }

    // The pages below are rebuilt from MONITORINFO::rcWork, so their work
    // areas no longer contain our previous Dock reservation. Discard the old
    // rectangles before ApplyDockWorkAreaReservation() runs; otherwise it
    // "restores" that stale reservation into the fresh work area and can
    // expand it across the Windows taskbar.
    dockAreas_.clear();
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
        fb.visualWorkArea = fb.workArea;
        gridPages_.push_back(fb);
    }

    // 从枚举结果提取系统主屏 monitorId（供双锚点回退解析使用）
    primaryMonitorId_.clear();
    for (const auto& p : gridPages_)
        if (p.isPrimary) { primaryMonitorId_ = p.monitorId; break; }
    if (primaryMonitorId_.empty() && !gridPages_.empty())
        primaryMonitorId_ = gridPages_.front().monitorId;

    std::sort(gridPages_.begin(), gridPages_.end(), [](const GridPage& a, const GridPage& b) {
        return a.bounds.left < b.bounds.left;
    });

    for (auto& page : gridPages_)
    {
        page.workArea.left   = std::clamp<LONG>(page.workArea.left,   0, static_cast<LONG>(virtualWidth_));
        page.workArea.top    = std::clamp<LONG>(page.workArea.top,    0, static_cast<LONG>(virtualHeight_));
        page.workArea.right  = std::clamp<LONG>(page.workArea.right,  page.workArea.left, static_cast<LONG>(virtualWidth_));
        page.workArea.bottom = std::clamp<LONG>(page.workArea.bottom, page.workArea.top,  static_cast<LONG>(virtualHeight_));
        page.visualWorkArea = page.workArea;
        ConfigureGridPage(page);
        ApplyIconSpacingToPage(page);
    }

    ApplyPageMapping();
    ApplyDockWorkAreaReservation();
}

/**
 * @brief 根据工作区尺寸配置网格页面的默认列数与行数。
 * @param page 待配置的网格页面。
 */
void DesktopApp::ConfigureGridPage(GridPage& page) const
{
    // Work areas are physical pixels; the initial cell and margins are DIPs.
    // Existing page dimensions still take precedence in ApplyPageMapping().
    const int w  = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
    const int h  = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
    page.columns = snowdesktop::grid_spacing_rules::InitialAxisCount(
        w, page.dpiX, kGridMarginX, kCellWidth, 4);
    page.rows = snowdesktop::grid_spacing_rules::InitialAxisCount(
        h, page.dpiY, kGridMarginY, kMinCellHeight, 3);
}

/**
 * @brief 将保存的列/行数设置应用到当前网格页面。
 */
void DesktopApp::ApplySavedGridDimensions()
{
    for (auto& page : gridPages_)
    {
        auto colIt = savedPageColumns_.find(page.id);
        auto rowIt = savedPageRows_.find(page.id);
        if (colIt != savedPageColumns_.end() && rowIt != savedPageRows_.end() &&
            colIt->second >= 1 && rowIt->second >= 1)
        {
            page.columns = colIt->second;
            page.rows = rowIt->second;
            ApplyIconSpacingToPage(page);
        }
    }
}

/**
 * @brief 根据固定行列数与图标间距比例重新计算页面布局。
 * @param page 目标网格页面。
 */
void DesktopApp::ApplyIconSpacingToPage(GridPage& page)
{
    page.columns = std::max(1, page.columns);
    page.rows = std::max(1, page.rows);

    const int pageW = static_cast<int>(std::max<LONG>(
        1, page.workArea.right - page.workArea.left));
    const int pageH = static_cast<int>(std::max<LONG>(
        1, page.workArea.bottom - page.workArea.top));
    const RECT visualArea = IsRectEmptyRect(page.visualWorkArea)
        ? page.workArea : page.visualWorkArea;
    const int visualW = static_cast<int>(std::max<LONG>(
        1, visualArea.right - visualArea.left));
    const int visualH = static_cast<int>(std::max<LONG>(
        1, visualArea.bottom - visualArea.top));

    const float pageVisualScale = std::max(0.1f, std::min(
        static_cast<float>(visualW) /
            static_cast<float>(page.columns * kCellWidth),
        static_cast<float>(visualH) /
            static_cast<float>(page.rows * kMinCellHeight)));
    const int baseMarginX = std::max(1, static_cast<int>(
        std::round(kGridMarginX * pageVisualScale)));
    const int baseMarginY = std::max(1, static_cast<int>(
        std::round(kGridMarginY * pageVisualScale)));

    const int innerWidth = std::max(
        page.columns, visualW - baseMarginX * 2);
    const int innerHeight = std::max(
        page.rows, visualH - baseMarginY * 2);
    page.itemPitchWidth = std::max(
        1, static_cast<int>(std::round(
            static_cast<float>(innerWidth) /
            static_cast<float>(page.columns))));
    page.itemPitchHeight = std::max(
        1, static_cast<int>(std::round(
            static_cast<float>(innerHeight) /
            static_cast<float>(page.rows))));
    const auto visualMetrics = GetPageItemVisualMetrics(page);

    const auto horizontal = snowdesktop::grid_spacing_rules::ResolveAxis(
        pageW, page.columns, baseMarginX, kGapPercentX,
        visualMetrics.minimumGridWidth, iconSpacingScale_);
    const auto vertical = snowdesktop::grid_spacing_rules::ResolveAxis(
        pageH, page.rows, baseMarginY, kGapPercentY,
        visualMetrics.minimumGridHeight, iconSpacingScale_);
    page.marginX = horizontal.margin;
    page.cellWidth = horizontal.cell;
    page.gapX = horizontal.gap;
    page.marginY = vertical.margin;
    page.cellHeight = vertical.cell;
    page.gapY = vertical.gap;
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
int DesktopApp::GetGridAxisIndexFromPoint(const GridPage& page, int coordinate, bool horizontal)
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
GridCell DesktopApp::CellFromPoint(POINT point) const
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
 * @brief 拖拽放置用的网格命中：按左上角边界包含而非中心距离，
 *        使图标左上角越过单元格边界即命中该格，消除半格偏移。
 */
GridCell DesktopApp::CellFromPointForDrag(POINT point) const
{
    const GridPage* page = GridPageFromPoint(point);
    GridCell cell;
    if (!page) return cell;
    cell.pageId = page->id;

    auto axisIndexForDrag = [&](bool horizontal) -> int {
        const int count = horizontal ? page->columns : page->rows;
        const int coordinate = horizontal ? point.x : point.y;
        const int margin = horizontal ? page->marginX : page->marginY;
        const int cellSize = horizontal ? page->cellWidth : page->cellHeight;
        const int origin = (horizontal ? page->workArea.left : page->workArea.top) + margin;

        for (int i = 0; i < count; ++i)
        {
            const int cellLeft = origin + GetGridAxisOffset(*page, i, horizontal);
            const int cellRight = cellLeft + cellSize;
            if (coordinate < cellRight)
                return i;
        }
        return count - 1;
    };

    cell.column = axisIndexForDrag(true);
    cell.row = axisIndexForDrag(false);
    return cell;
}

/**
 * @brief 检查网格区域是否被未选中的项目或组件占据。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 被占据返回 true。
 */
bool DesktopApp::IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const
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
        if (!snowdesktop::collection_group_rules::
                ShouldOccupyDesktopGrid(
                    IsGroupedWidget(w)))
            continue;
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
std::vector<DesktopApp::PendingGridMove> DesktopApp::BuildSelectedMove(GridCell targetCell) const
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
    const bool hasLargeIcon = std::any_of(selectedIndexes.begin(), selectedIndexes.end(),
        [&](size_t index) { return items_[index].largeIcon.has_value(); });
    if (hasLargeIcon && (targetCell.column < 0 || targetCell.row < 0 ||
        targetCell.column + spreadCols > page->columns || targetCell.row + groupRows > page->rows)) return moves;
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
            movedCell.column + items_[itemIndex].gridSpan.columns > page->columns ||
            movedCell.row + items_[itemIndex].gridSpan.rows > page->rows ||
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
GridCell DesktopApp::FindBestDropCell(const DragSourceList& sourceList, GridCell targetCell) const
{
    const bool exactPlacement = !containers_.empty() &&
        sourceList.origin == containers_.front().get() &&
        std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
            [&](const DragSourceEntry& entry) {
                return entry.kind == DropSourceKind::DesktopIcon && !entry.fromDock &&
                    entry.desktopIndex < items_.size() &&
                    items_[entry.desktopIndex].largeIcon.has_value();
            });
    const POINT current = dragSession_.CurrentPoint();
    const POINT mouseDown = dragSession_.MouseDownPoint();
    const auto direction =
        snowdesktop::desktop_drop_cache::
            ResolveSearchDirection(
                static_cast<long long>(current.x) -
                    mouseDown.x,
                static_cast<long long>(current.y) -
                    mouseDown.y);
    const snowdesktop::desktop_drop_cache::BestCellKey cacheKey{
        targetCell,
        direction,
        dragSession_.StaticSceneRevision() };
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    return snowdesktop::desktop_drop_cache::FindBestCell(
        bestDropCellCache_, dragSession_.IsActive(), cacheKey,
        exactPlacement, page ? page->columns : 0, page ? page->rows : 0,
        [this](const GridCell& cell) { return !BuildSelectedMove(cell).empty(); });
}

/**
 * @brief 将选中的项目移动到目标网格单元格。
 * @param targetCell 目标单元格。
 */
void DesktopApp::MoveSelectedItemsToCell(GridCell targetCell)
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
void DesktopApp::UpdateDragGroupOrigin()
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
    const GridPage* firstPage = GetFirstPageGridPage();
    groupOrigin.pageId = anchorPageId.empty()
        ? (firstPage ? firstPage->id : L"")
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
void DesktopApp::MigrateSelectedItemsToLastMonitorPage()
{
    // Page turning during a large-icon drag is a preview only. Retain the
    // entire group's source cells/spans until its accepted drop commits them;
    // this also makes cancellation leave the original layout intact.
    if (std::any_of(items_.begin(), items_.end(), [](const auto& item) { return item.selected && item.largeIcon; })) return;
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
        if (!IsGroupedWidget(w))
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
