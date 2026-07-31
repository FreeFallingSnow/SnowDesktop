#include "app.h"

// Desktop and widget sorting plus clipboard cut-state updates.

void DesktopApp::SortIconsByName(bool ascending)
{
    auto sortForPage = [&](const GridPage& page) {
        const GridPage* firstPage = GetFirstPageGridPage();
        std::vector<size_t> order;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].name.empty() || IsItemInAnyWidget(items_[i])) continue;
            if (items_[i].gridCell.pageId.empty())
                items_[i].gridCell.pageId = firstPage ? firstPage->id : L"";
            if (items_[i].gridCell.pageId == page.id)
                order.push_back(i);
        }

        std::sort(order.begin(), order.end(), [this, ascending](size_t a, size_t b) {
            int cmp = ToUpperInvariant(items_[a].name).compare(ToUpperInvariant(items_[b].name));
            return ascending ? (cmp < 0) : (cmp > 0);
        });

        std::unordered_set<std::wstring> usedSlots;
        for (const auto& widget : widgets_)
            if (!IsGroupedWidget(widget) &&
                widget.gridCell.pageId == page.id)
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
void DesktopApp::SortIconsByType(bool ascending)
{
    auto sortForPage = [&](const GridPage& page) {
        const GridPage* firstPage = GetFirstPageGridPage();
        std::vector<size_t> order;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].name.empty() || IsItemInAnyWidget(items_[i])) continue;
            if (items_[i].gridCell.pageId.empty())
                items_[i].gridCell.pageId = firstPage ? firstPage->id : L"";
            if (items_[i].gridCell.pageId == page.id)
                order.push_back(i);
        }

        std::sort(order.begin(), order.end(), [this, ascending](size_t a, size_t b) {
            int cmp = ToUpperInvariant(items_[a].typeName).compare(ToUpperInvariant(items_[b].typeName));
            if (cmp != 0) return ascending ? (cmp < 0) : (cmp > 0);
            cmp = ToUpperInvariant(items_[a].name).compare(ToUpperInvariant(items_[b].name));
            return ascending ? (cmp < 0) : (cmp > 0);
        });

        std::unordered_set<std::wstring> usedSlots;
        for (const auto& widget : widgets_)
            if (!IsGroupedWidget(widget) &&
                widget.gridCell.pageId == page.id)
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
void DesktopApp::SortWidgetContents(size_t widgetIndex, int mode, bool ascending)
{
    if (widgetIndex >= widgets_.size()) return;
    DesktopWidget& w = widgets_[widgetIndex];

    if (w.type == DesktopWidgetType::CollectionGroup)
    {
        std::wstring active = w.activeCategoryId;
        if (std::find(
                w.childWidgetIds.begin(),
                w.childWidgetIds.end(), active) ==
            w.childWidgetIds.end())
            active = w.childWidgetIds.empty()
                ? L""
                : w.childWidgetIds.front();
        const size_t activeIndex =
            FindWidgetIndexById(active);
        if (activeIndex < widgets_.size() &&
            widgets_[activeIndex].type ==
                DesktopWidgetType::Collection)
            SortWidgetContents(
                activeIndex, mode, ascending);
        return;
    }

    if (w.type == DesktopWidgetType::FolderMapping)
    {
        w.folderSortMode =
            snowdesktop::folder_sort_rules::
                NormalizeMode(mode);
        w.folderSortAscending = ascending;
        snowdesktop::folder_sort_rules::StableSort(
            w.folderEntries,
            w.folderSortMode,
            w.folderSortAscending);
        snowdesktop::folder_sort_rules::
            RewriteOrderKeys(
                w.folderEntries, w.itemKeys);
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
            [this, mode, ascending](const std::wstring& ka, const std::wstring& kb) {
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
                        if (timeCmp != 0) cmp = timeCmp;
                        else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                    }
                    else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                return ascending ? (cmp < 0) : (cmp > 0);
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
            [this, mode, ascending](const std::wstring& ka, const std::wstring& kb) {
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
                        if (timeCmp != 0) cmp = timeCmp;
                        else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                    }
                    else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                return ascending ? (cmp < 0) : (cmp > 0);
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
void DesktopApp::UpdateCutState()
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
    if (dockFolderPopupOpen_)
    {
        for (auto& entry :
             dockFolderPopupWidget_.
                folderEntries)
        {
            entry.isCut = false;
            if (!entry.fullPath.empty() &&
                clipCutPaths.contains(
                    ToUpperInvariant(
                        entry.fullPath)))
                entry.isCut = true;
        }
    }
}

// ── Shell 变更通知 ──────────────────────────────────────────

/**
 * @brief 注册 Shell 变更通知（文件创建、删除、重命名、属性变更等），用于实时刷新桌面。
 */
