#include "app.h"

// OLE data-object construction and desktop landing bookkeeping.

POINT DesktopApp::GetDragTargetPoint(POINT current) const
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
ComPtr<IDataObject> DesktopApp::CreateSelectedDataObject() const
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
ComPtr<IDataObject> DesktopApp::CreateFileDropDataObject(const std::vector<std::wstring>& paths)
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
ComPtr<IDataObject> DesktopApp::CreateDataObjectForItems(
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
void DesktopApp::DropSelectedItemsOnTarget(int targetIndex)
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
size_t DesktopApp::FindItemIndexByKey(const std::wstring& key) const
{
    std::wstring normalized = ToUpperInvariant(key);
    auto it = itemIndexByKeyCache_.find(normalized);
    if (it != itemIndexByKeyCache_.end() && it->second < items_.size())
        return it->second;
    return static_cast<size_t>(-1);
}

void DesktopApp::RefreshDesktopItemIndexCache()
{
    itemIndexByKeyCache_.clear();
    itemIndexByKeyCache_.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (!items_[i].layoutKey.empty())
            itemIndexByKeyCache_.emplace(ToUpperInvariant(items_[i].layoutKey), i);
    }
}

void DesktopApp::RefreshCollectedKeysCache()
{
    collectedKeysCache_.clear();
    for (const auto& widget : widgets_)
    {
        for (const auto& key : widget.itemKeys)
            if (!key.empty())
                collectedKeysCache_.insert(ToUpperInvariant(key));
    }
    for (const auto& entry : dockEntries_)
    {
        if (entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            !entry.reference.empty())
            collectedKeysCache_.insert(ToUpperInvariant(entry.reference));
    }
}

/**
 * @brief 从所有组件中移除指定桌面键。
 * @param keys 要移除的布局键列表。
 */
void DesktopApp::RemoveDesktopKeysFromWidgets(const std::vector<std::wstring>& keys)
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
    RefreshCollectedKeysCache();
}

/**
 * @brief 快照当前所有桌面项的布局键。
 * @return 布局键的集合。
 */
std::unordered_set<std::wstring> DesktopApp::SnapshotDesktopKeys() const
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
std::vector<std::wstring> DesktopApp::NewDesktopKeysSince(
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
std::vector<DropLanding> DesktopApp::BuildDesktopLandings(
    const DragSourceList& sourceList, GridCell targetCell, bool internalMove) const
{
    std::vector<DropLanding> landings;
    if (sourceList.Empty()) return landings;

    if (targetCell.pageId.empty())
    {
        const GridPage* firstPage = GetFirstPageGridPage();
        if (firstPage) targetCell.pageId = firstPage->id;
    }
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
            landing.span = entry.originalSpan;
            landings.push_back(landing);
        }
        return landings;
    }

    // 起始列：拖放目标列，换行时从该列另起一行而非从 0 开始
    const int startCol = std::clamp(targetCell.column, 0, std::max(0, page->columns - 1));

    auto advanceCell = [&](GridCell cell, GridSpan span) {
        // 查找 cell 所在页的维度（支持跨页后 cursor 切到新页）
        int cols = page->columns, rows = page->rows;
        if (cell.pageId != page->id)
        {
            auto colIt = savedPageColumns_.find(cell.pageId);
            auto rowIt = savedPageRows_.find(cell.pageId);
            if (colIt != savedPageColumns_.end()) cols = colIt->second;
            if (rowIt != savedPageRows_.end()) rows = rowIt->second;
        }
        cell.column += std::max(1, span.columns);
        if (cell.column + span.columns > cols)
        {
            cell.column = startCol;
            cell.row += std::max(1, span.rows);
            // 到底后绕回起始行上方（搜索阶段会跳过已占位置）
            if (cell.row + span.rows > rows)
                cell.row = 0;
        }
        return cell;
    };

    std::unordered_set<std::wstring> sourceKeys;
    for (const auto& entry : sourceList.entries)
        if (!entry.desktopKey.empty())
            sourceKeys.insert(ToUpperInvariant(entry.desktopKey));

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (internalMove && sourceKeys.contains(ToUpperInvariant(item.layoutKey))) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell cursor = targetCell;

    // 阶段式搜索：1) 从 cursor 向右 + 下方行从 startCol 开始
    //             1b) 上方行从 startCol 开始（绕回页面顶部填间隙）
    //             1c) 下方/上方行从列 0..startCol-1 补扫（覆盖左侧空位）
    //             2) 全页行优先搜索（兜底）
    //             3) TryFindFreeCell 跨页搜索
    auto tryPlaceRightward = [&](GridSpan span, GridCell fromCell, GridCell& outCell) -> bool {
        // 获取 fromCell 所在页的维度（支持跨页后 cursor 切到新页）
        int pageCols = page->columns, pageRows = page->rows;
        if (fromCell.pageId != page->id)
        {
            auto colIt = savedPageColumns_.find(fromCell.pageId);
            auto rowIt = savedPageRows_.find(fromCell.pageId);
            if (colIt != savedPageColumns_.end()) pageCols = colIt->second;
            if (rowIt != savedPageRows_.end()) pageRows = rowIt->second;
        }

        // 当前行从 fromCell.column 向右找
        for (int c = fromCell.column; c + span.columns <= pageCols; ++c)
        {
            GridCell candidate{ fromCell.pageId, c, fromCell.row };
            if (!AreGridSlotsMarked(usedSlots, candidate, span))
            {
                outCell = candidate;
                return true;
            }
        }

        // 下方行：先从 startCol 向右，再从 0..startCol-1 补扫
        for (int r = fromCell.row + 1; r + span.rows <= pageRows; ++r)
        {
            for (int c = startCol; c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
            for (int c = 0; c < startCol && c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
        }

        // 上方行：先从 startCol 向右，再从 0..startCol-1 补扫
        for (int r = 0; r < fromCell.row && r + span.rows <= pageRows; ++r)
        {
            for (int c = startCol; c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
            for (int c = 0; c < startCol && c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
        }
        return false;
    };

    for (const auto& entry : sourceList.entries)
    {
        GridSpan span = entry.originalSpan;
        span.columns = std::max(1, span.columns);
        span.rows = std::max(1, span.rows);

        GridCell cell{};
        bool found = false;

        // 获取 cursor 所在页的维度（支持跨页后 cursor 切到新页）
        auto getPageSize = [&](const std::wstring& pid) -> std::pair<int,int> {
            if (pid == page->id) return { page->columns, page->rows };
            auto colIt = savedPageColumns_.find(pid);
            auto rowIt = savedPageRows_.find(pid);
            int c = (colIt != savedPageColumns_.end()) ? colIt->second : page->columns;
            int r = (rowIt != savedPageRows_.end()) ? rowIt->second : page->rows;
            return { c, r };
        };

        // 阶段 1：从 cursor 向右 + 下方行从 startCol
        auto [curCols, curRows] = getPageSize(cursor.pageId);
        if (IsGridAreaValid(cursor, span) && cursor.column + span.columns <= curCols &&
            cursor.row + span.rows <= curRows &&
            !AreGridSlotsMarked(usedSlots, cursor, span))
        {
            cell = cursor;
            found = true;
        }
        else
        {
            found = tryPlaceRightward(span, cursor, cell);
        }

        // 阶段 2：全页行优先搜索（兜底，优先本页）
        if (!found)
        {
            for (int r = 0; r + span.rows <= curRows && !found; ++r)
            {
                for (int c = 0; c + span.columns <= curCols && !found; ++c)
                {
                    GridCell candidate{ cursor.pageId, c, r };
                    if (!AreGridSlotsMarked(usedSlots, candidate, span))
                    {
                        cell = candidate;
                        found = true;
                    }
                }
            }
        }

        // 阶段 3：跨页搜索（TryFindFreeCell 内部遍历其他显示页）
        if (!found)
        {
            found = TryFindFreeCell(span, usedSlots, cell, cursor.pageId, 0);
        }

        // 阶段 4：所有现有页都满了 → 预分配新溢出页
        if (!found)
        {
            std::wstring newPageId = GeneratePageId();
            // GeneratePageId 保证唯一，savedPageColumns_ 不会有该页
            if (!savedPageColumns_.count(newPageId))
            {
                cell = { newPageId, 0, 0 };
                found = true;
                // 新页维度由 ApplyPendingPlacement 创建时参考末屏设置
            }
        }

        if (!found) continue;

        DropLanding landing;
        landing.kind = DropLandingKind::DesktopCell;
        landing.sourceIndex = entry.sourceIndex;
        landing.cell = cell;
        landing.span = span;
        landings.push_back(landing);

        MarkGridArea(usedSlots, cell, span);
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
