#pragma once
// Inline implementations for DesktopApp — Grid helpers, Shell, Filtering,
// Layout persistence, Control window, Bitmap cache, Data loading, and free functions.
// This file is included by app_oo.h after the class definition.

// ── Grid helpers ────────────────────────────────────────────

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

inline void DesktopApp::SetFirstPageMonitorFromPoint(POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* page = GridPageFromPoint(clientPoint);
    if (!page || page->monitorId.empty()) return;

    firstPageMonitorId_ = page->monitorId;
    pageOffset_ = 0;
    ApplyPageMapping();
    ApplySavedGridDimensions();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

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

inline bool DesktopApp::PageHasContent(const std::wstring& pageId) const
{
    if (pageId.empty()) return false;
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId == pageId) return true;
    for (const auto& w : widgets_)
        if (w.gridCell.pageId == pageId) return true;
    return false;
}

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
}

inline void DesktopApp::MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
{
    for (int c = cell.column; c < cell.column + span.columns; ++c)
        for (int r = cell.row; r < cell.row + span.rows; ++r)
            usedSlots.insert(cell.pageId + L":" + std::to_wstring(c) + L"," + std::to_wstring(r));
}

inline bool DesktopApp::AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
{
    for (int c = cell.column; c < cell.column + span.columns; ++c)
        for (int r = cell.row; r < cell.row + span.rows; ++r)
            if (usedSlots.count(cell.pageId + L":" + std::to_wstring(c) + L"," + std::to_wstring(r)))
                return true;
    return false;
}

inline bool DesktopApp::IsGridAreaValid(const GridCell& cell, GridSpan span)
{
    if (span.columns < 1 || span.rows < 1) return false;
    if (cell.column < 0 || cell.row < 0) return false;
    return true;
}

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
            if (IsGridAreaValid(candidate, span) && !AreGridSlotsMarked(usedSlots, candidate, span))
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

inline void DesktopApp::RelayoutDisplacedItems()
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    std::unordered_set<std::wstring> placedKeys;
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        const GridPage* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (page &&
            item.gridCell.column + item.gridSpan.columns <= page->columns &&
            item.gridCell.row + item.gridSpan.rows <= page->rows)
        {
            std::wstring key = item.gridCell.pageId + L":" +
                std::to_wstring(item.gridCell.column) + L"," + std::to_wstring(item.gridCell.row);
            if (!AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan))
            {
                MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                placedKeys.insert(item.layoutKey);
                continue;
            }
        }

        GridCell freeCell;
        if (TryFindFreeCell(item.gridSpan, usedSlots, freeCell, item.gridCell.pageId))
        {
            item.gridCell = freeCell;
            MarkGridArea(usedSlots, freeCell, item.gridSpan);
        }
    }
}

inline void DesktopApp::SortIconsByName()
{
    std::vector<size_t> order;
    order.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) order.push_back(i);

    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        return ToUpperInvariant(items_[a].name) < ToUpperInvariant(items_[b].name);
    });

    int idx = 0;
    for (size_t i : order)
    {
        if (!gridPages_.empty() && items_[i].gridCell.pageId.empty())
            items_[i].gridCell.pageId = gridPages_.front().id;
        items_[i].gridCell.column = idx / std::max(1, gridPages_.front().rows);
        items_[i].gridCell.row = idx % std::max(1, gridPages_.front().rows);
        items_[i].gridSpan = {1, 1};
        ++idx;
    }

    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::SortIconsByType()
{
    std::vector<size_t> order;
    order.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) order.push_back(i);

    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        int cmp = ToUpperInvariant(items_[a].typeName).compare(ToUpperInvariant(items_[b].typeName));
        if (cmp != 0) return cmp < 0;
        return ToUpperInvariant(items_[a].name) < ToUpperInvariant(items_[b].name);
    });

    int idx = 0;
    for (size_t i : order)
    {
        if (!gridPages_.empty() && items_[i].gridCell.pageId.empty())
            items_[i].gridCell.pageId = gridPages_.front().id;
        items_[i].gridCell.column = idx / std::max(1, gridPages_.front().rows);
        items_[i].gridCell.row = idx % std::max(1, gridPages_.front().rows);
        items_[i].gridSpan = {1, 1};
        ++idx;
    }

    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

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
                        clipCutPaths.insert(path);
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
            if (clipCutPaths.contains(path))
                item.isCut = true;
        }
    }
}

// ── Shell change notifications ──────────────────────────────

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

// ── Filtering ───────────────────────────────────────────────

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

// ── Layout persistence ──────────────────────────────────────

inline std::wstring DesktopApp::GetLayoutPath() const
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"SnowDesktop.layout.json");
    return path;
}

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

inline void DesktopApp::RememberSavedPageId(const std::wstring& pageId)
{
    if (pageId.empty()) return;
    if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
        savedPageIds_.push_back(pageId);
}

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
                        int x = 0, y = 0, w = 1, h = 1, scrollOffset = 0;
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
                        ReadJsonBoolField(obj, "autoCollect", autoCollect);
                        ReadJsonBoolField(obj, "listMode", listMode);

                        DesktopWidget widget;
                        widget.id = Utf8ToWide(idUtf8);
                        widget.type = WidgetTypeFromJson(Utf8ToWide(typeUtf8));
                        widget.title = titleUtf8.empty()
                            ? (widget.type == DesktopWidgetType::FileCategories ? L"桌面文件"
                               : widget.type == DesktopWidgetType::FolderMapping ? L"文件夹映射"
                               : L"集合")
                            : Utf8ToWide(titleUtf8);
                        widget.sourceFolderPath = Utf8ToWide(sourceUtf8);
                        widget.scriptPath = Utf8ToWide(scriptUtf8);
                        widget.gridCell.pageId = Utf8ToWide(pageUtf8);
                        widget.gridCell.column = x;
                        widget.gridCell.row = y;
                        widget.gridSpan.columns = std::max(1, w);
                        widget.gridSpan.rows = std::max(1, h);
                        widget.autoCollect = autoCollect;
                        widget.listMode = listMode;
                        widget.showTitle = true;
                        widget.bottomBarHover = (widget.type == DesktopWidgetType::Collection);
                        ReadJsonBoolField(obj, "showTitle", widget.showTitle);
                        ReadJsonBoolField(obj, "bottomBarHover", widget.bottomBarHover);
                        widget.scrollOffset = std::max(0, scrollOffset);
                        widget.activeCategoryId = Utf8ToWide(activeCategoryUtf8);
                        ReadJsonStringArrayField(obj, "items", widget.itemKeys);

                        widgets_.push_back(std::move(widget));
                        wp = objectEnd + 1;
                    }
                }
            }
        }
    }
}

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
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto* it = sorted[i];
        file << "    { \"key\": \"" << JsonEscapeUtf8(it->layoutKey)
             << "\", \"page\": \"" << JsonEscapeUtf8(it->gridCell.pageId)
             << "\", \"x\": " << it->gridCell.column
             << ", \"y\": " << it->gridCell.row
             << ", \"w\": " << std::max(1, it->gridSpan.columns)
             << ", \"h\": " << std::max(1, it->gridSpan.rows)
             << ", \"slot\": " << it->slot << " }";
        file << (i + 1 == sorted.size() ? "\n" : ",\n");
    }
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
             << ", \"showTitle\": " << (w.showTitle ? "true" : "false")
             << ", \"bottomBarHover\": " << (w.bottomBarHover ? "true" : "false")
             << ", \"scrollOffset\": " << std::max(0, w.scrollOffset)
             << ", \"items\": [";
        for (size_t j = 0; j < w.itemKeys.size(); ++j)
        {
            file << "\"" << JsonEscapeUtf8(w.itemKeys[j]) << "\"";
            if (j + 1 != w.itemKeys.size()) file << ", ";
        }
        file << "] }";
        file << (i + 1 == widgets_.size() ? "\n" : ",\n");
    }
    file << "  ]\n}\n";
}

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

inline size_t DesktopApp::FindJsonObjectEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '{', '}'); }

inline size_t DesktopApp::FindJsonArrayEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '[', ']'); }

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

inline DesktopWidgetType DesktopApp::WidgetTypeFromJson(const std::wstring& type) const
{
    std::wstring n = ToUpperInvariant(type);
    if (n == L"FILECATEGORIES" || n == L"FILE_CATEGORIES") return DesktopWidgetType::FileCategories;
    if (n == L"FOLDERMAPPING" || n == L"FOLDER_MAPPING") return DesktopWidgetType::FolderMapping;
    if (n == L"LUA" || n == L"LUASCRIPT" || n == L"LUA_SCRIPT") return DesktopWidgetType::LuaScript;
    if (n == L"COLLECTION") return DesktopWidgetType::Collection;
    return DesktopWidgetType::Collection;
}

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

// ── Control window ──────────────────────────────────────────

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

inline LRESULT DesktopApp::HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (taskbarRestartMsg_ && msg == taskbarRestartMsg_)
    {
        AddTrayIcon(true);
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
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        controlHwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Bitmap cache ────────────────────────────────────────────

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

inline void DesktopApp::ReloadItems(bool reloadLayoutFromDisk)
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    if (reloading_) return;
    reloading_ = true;
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    if (reloadLayoutFromDisk)
        LoadLayoutSlots();
    LoadDesktopItems();

    // Mark widgets as used
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    // Mark items with valid existing positions as used; flag unslotted items
    std::unordered_set<std::wstring> placedKeys;
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
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
        if (!item.name.empty() && item.gridCell.pageId.empty())
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
    SaveLayoutSlots();
    UpdateCutState();
    RebuildContainersAndItems();
    reloading_ = false;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::LoadDesktopItems()
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    auto L = [](const wchar_t* s) {
        HANDLE f = CreateFileW(L"SnowDesktopOO_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
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

    ApplySavedGridDimensions();
    ApplyPageMapping();
}

inline void DesktopApp::ConfigureGridPage(GridPage& page) const
{
    const int cw = static_cast<int>(kCellWidth * gapScale_);
    const int ch = static_cast<int>(kMinCellHeight * gapScale_);
    const int w  = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
    const int h  = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
    const int uw = std::max(1, w - page.marginX * 2);
    const int uh = std::max(1, h - page.marginY * 2);
    page.columns   = std::max(4, uw / cw);
    page.rows      = std::max(3, uh / ch);
    page.cellWidth  = cw;
    page.cellHeight = ch;
    page.gapX = page.columns > 1 ? std::max(0, (uw - page.columns * page.cellWidth)  / (page.columns - 1)) : 0;
    page.gapY = page.rows    > 1 ? std::max(0, (uh - page.rows    * page.cellHeight) / (page.rows    - 1)) : 0;
}

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

inline void DesktopApp::ApplyGapScaleToPage(GridPage& page)
{
    const int usableW = std::max(1, static_cast<int>(page.workArea.right - page.workArea.left) - (page.marginX * 2));
    const int usableH = std::max(1, static_cast<int>(page.workArea.bottom - page.workArea.top) - (page.marginY * 2));
    const float cellRefW = static_cast<float>(usableW) / static_cast<float>(std::max(1, page.columns));
    const float cellRefH = static_cast<float>(usableH) / static_cast<float>(std::max(1, page.rows));
    const int targetGapX = std::max(0, static_cast<int>(cellRefW * kGapPercentX / gapScale_));
    const int targetGapY = std::max(0, static_cast<int>(cellRefH * kGapPercentY / gapScale_));

    page.cellWidth = page.columns > 1
        ? std::max(kIconSize, (usableW - (page.columns - 1) * targetGapX) / page.columns)
        : usableW;
    page.cellHeight = page.rows > 1
        ? std::max(kMinCellHeight / 2, (usableH - (page.rows - 1) * targetGapY) / page.rows)
        : usableH;
    page.gapX = page.columns > 1 ? (usableW - page.columns * page.cellWidth) / (page.columns - 1) : 0;
    page.gapY = page.rows    > 1 ? (usableH - page.rows    * page.cellHeight) / (page.rows - 1) : 0;
}

// ── Drag helpers ──────────────────────────────────────────────

extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
extern inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal);
extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);
extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);

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

inline bool DesktopApp::IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const
{
    for (const auto& item : items_)
    {
        if (item.selected || item.name.empty()) continue;
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

inline GridCell DesktopApp::FindBestDropCell(GridCell targetCell) const
{
    if (!BuildSelectedMove(targetCell).empty()) return targetCell;

    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return targetCell;
    const int maxCol = page->columns - 1;
    const int maxRow = page->rows - 1;

    int dx = dragCurrentPoint_.x - mouseDownPoint_.x;
    int dy = dragCurrentPoint_.y - mouseDownPoint_.y;
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

inline POINT DesktopApp::GetDragTargetPoint(POINT current) const
{
    return {
        dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
        dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
    };
}

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

    POINT screenPt = dragCurrentPoint_;
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

inline size_t DesktopApp::FindItemIndexByKey(const std::wstring& key) const
{
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].layoutKey == key) return i;
    return static_cast<size_t>(-1);
}

inline void DesktopApp::CopySelectedItemsToCell(GridCell targetCell)
{
    std::unordered_set<std::wstring> existingKeys;
    for (const auto& item : items_)
        if (!item.layoutKey.empty()) existingKeys.insert(item.layoutKey);

    wchar_t desktopPath[MAX_PATH]{};
    SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);

    for (const auto& item : items_)
    {
        if (!item.selected || !item.desktopIconClsid.empty()) continue;
        wchar_t srcPath[MAX_PATH]{};
        if (!SHGetPathFromIDListW(item.absolutePidl.get(), srcPath)) continue;

        wchar_t nameNoExt[MAX_PATH]{};
        wcscpy_s(nameNoExt, PathFindFileNameW(srcPath));
        PathRemoveExtensionW(nameNoExt);
        wchar_t ext[MAX_PATH]{};
        wcscpy_s(ext, PathFindExtensionW(srcPath));

        wchar_t newName[MAX_PATH]{};
        swprintf_s(newName, L"%s - 副本%s", nameNoExt, ext);
        wchar_t dstPath[MAX_PATH]{};
        PathCombineW(dstPath, desktopPath, newName);

        if (GetFileAttributesW(dstPath) != INVALID_FILE_ATTRIBUTES)
        {
            for (int i = 2; i < 100; ++i)
            {
                swprintf_s(newName, L"%s - 副本 (%d)%s", nameNoExt, i, ext);
                PathCombineW(dstPath, desktopPath, newName);
                if (GetFileAttributesW(dstPath) == INVALID_FILE_ATTRIBUTES) break;
            }
        }

        std::wstring dstDir = desktopPath;
        if (!dstDir.empty() && dstDir.back() != L'\\') dstDir += L'\\';
        dstDir += newName;

        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_COPY;
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION;
        wchar_t from[MAX_PATH + 2]{};
        wcscpy_s(from, srcPath);
        from[wcslen(srcPath) + 1] = L'\0';
        op.pFrom = from;
        op.pTo = desktopPath;
        SHFileOperationW(&op);

        if (GetFileAttributesW(dstPath) == INVALID_FILE_ATTRIBUTES)
        {
            swprintf_s(newName, L"%s - 副本 (%d)%s", nameNoExt, 2, ext);
            PathCombineW(dstPath, desktopPath, newName);
        }
    }

    ReloadItems(false);
    PlaceNewItemsAtDropPoint(existingKeys, targetCell);
}

inline void DesktopApp::CreateShortcutSelectedItemsToCell(GridCell targetCell)
{
    std::unordered_set<std::wstring> existingKeys;
    for (const auto& item : items_)
        if (!item.layoutKey.empty()) existingKeys.insert(item.layoutKey);

    wchar_t desktopPath[MAX_PATH]{};
    SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);

    for (const auto& item : items_)
    {
        if (!item.selected || !item.desktopIconClsid.empty()) continue;
        wchar_t srcPath[MAX_PATH]{};
        if (!SHGetPathFromIDListW(item.absolutePidl.get(), srcPath)) continue;

        wchar_t nameNoExt[MAX_PATH]{};
        wcscpy_s(nameNoExt, PathFindFileNameW(srcPath));
        PathRemoveExtensionW(nameNoExt);

        wchar_t lnkPath[MAX_PATH]{};
        PathCombineW(lnkPath, desktopPath, (std::wstring(nameNoExt) + L".lnk").c_str());
        if (GetFileAttributesW(lnkPath) != INVALID_FILE_ATTRIBUTES)
        {
            for (int i = 2; i < 100; ++i)
            {
                swprintf_s(lnkPath, L"%s\\%s (%d).lnk", desktopPath, nameNoExt, i);
                if (GetFileAttributesW(lnkPath) == INVALID_FILE_ATTRIBUTES) break;
            }
        }

        ComPtr<IShellLinkW> shellLink;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
        {
            shellLink->SetPath(srcPath);
            shellLink->SetWorkingDirectory(desktopPath);
            ComPtr<IPersistFile> persistFile;
            if (SUCCEEDED(shellLink.As(&persistFile)))
                persistFile->Save(lnkPath, TRUE);
        }
    }

    ReloadItems(false);
    PlaceNewItemsAtDropPoint(existingKeys, targetCell);
}

inline void DesktopApp::ApplyPendingPlacement()
{
    if (!hasPendingPlace_) return;
    if (GetTickCount() - pendingPlaceTick_ > 10000)
    {
        hasPendingPlace_ = false;
        pendingPlaceNames_.clear();
        return;
    }

    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);

    std::vector<std::wstring> srcFileNames;
    srcFileNames.reserve(pendingPlaceNames_.size());
    for (const auto& p : pendingPlaceNames_)
        srcFileNames.push_back(FileNameFromPath(p));

    GridCell startCell = pendingPlaceCell_;
    if (startCell.pageId.empty() && !gridPages_.empty())
        startCell.pageId = gridPages_.front().id;
    const GridPage* page = FindGridPage(gridPages_, startCell.pageId);
    if (!page) { hasPendingPlace_ = false; pendingPlaceNames_.clear(); return; }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty() || items_[i].gridCell.pageId.empty()) continue;
        bool isTarget = false;
        for (const auto& src : srcFileNames)
        {
            if (MatchPendingName(items_[i].name, src) ||
                (!items_[i].parsingName.empty() &&
                 MatchPendingName(FileNameFromPath(items_[i].parsingName), src)))
            {
                isTarget = true;
                break;
            }
        }
        if (!isTarget && items_[i].slot >= 0)
            MarkGridArea(usedSlots, items_[i].gridCell, items_[i].gridSpan);
    }

    struct Match { size_t itemIndex; size_t srcIndex; };
    std::vector<Match> matches;
    std::vector<bool> srcUsed(srcFileNames.size(), false);
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        for (size_t s = 0; s < srcFileNames.size(); ++s)
        {
            if (srcUsed[s]) continue;
            if (MatchPendingName(items_[i].name, srcFileNames[s]) ||
                (!items_[i].parsingName.empty() &&
                 MatchPendingName(FileNameFromPath(items_[i].parsingName), srcFileNames[s])))
            {
                matches.push_back({i, s});
                srcUsed[s] = true;
                break;
            }
        }
    }

    if (matches.empty()) return;

    int searchSlot = SlotFromCell(gridPages_, startCell);
    bool changed = false;
    GridCell nextCell = startCell;
    for (const auto& m : matches)
    {
        GridSpan span = items_[m.itemIndex].gridSpan;
        span.columns = std::max(1, span.columns);
        span.rows = std::max(1, span.rows);

        GridCell cell{};
        bool found = false;
        if (IsGridAreaValid(nextCell, span) && !AreGridSlotsMarked(usedSlots, nextCell, span))
        {
            cell = nextCell;
            found = true;
        }
        if (!found)
            found = TryFindFreeCell(span, usedSlots, cell, startCell.pageId, searchSlot);

        if (!found) continue;

        items_[m.itemIndex].gridCell = cell;
        items_[m.itemIndex].gridSpan = span;
        items_[m.itemIndex].slot = SlotFromCell(gridPages_, cell);
        items_[m.itemIndex].selected = true;
        MarkGridArea(usedSlots, cell, span);
        searchSlot = items_[m.itemIndex].slot + 1;
        nextCell = cell;
        nextCell.column += span.columns;
        if (nextCell.column >= page->columns)
        {
            nextCell.column = 0;
            nextCell.row += span.rows;
        }
        changed = true;
    }

    std::vector<std::wstring> remaining;
    for (size_t s = 0; s < srcFileNames.size(); ++s)
    {
        if (!srcUsed[s])
            remaining.push_back(pendingPlaceNames_[s]);
    }

    if (!remaining.empty())
    {
        pendingPlaceNames_ = std::move(remaining);
        pendingPlaceCell_ = nextCell;
    }
    else
    {
        hasPendingPlace_ = false;
        pendingPlaceNames_.clear();
    }

    if (changed)
    {
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

inline void DesktopApp::PlaceNewItemsAtDropPoint(
    const std::unordered_set<std::wstring>& existingKeys, GridCell targetCell)
{
    std::vector<size_t> newItems;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].layoutKey.empty()) continue;
        if (!existingKeys.contains(items_[i].layoutKey))
            newItems.push_back(i);
    }
    if (newItems.empty()) return;

    if (targetCell.pageId.empty() && !gridPages_.empty())
        targetCell.pageId = gridPages_.front().id;
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return;

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    std::unordered_set<size_t> newSet(newItems.begin(), newItems.end());
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (newSet.contains(i)) continue;
        MarkGridArea(usedSlots, items_[i].gridCell, items_[i].gridSpan);
    }

    ClearSelection();
    int searchSlot = SlotFromCell(gridPages_, targetCell);
    bool changed = false;
    for (size_t itemIndex : newItems)
    {
        GridSpan span = items_[itemIndex].gridSpan;
        span.columns = std::max(1, span.columns);
        span.rows = std::max(1, span.rows);

        GridCell cell{};
        bool found = false;
        if (IsGridAreaValid(targetCell, span) && !AreGridSlotsMarked(usedSlots, targetCell, span))
        {
            cell = targetCell;
            found = true;
        }
        else
        {
            found = TryFindFreeCell(span, usedSlots, cell, targetCell.pageId, searchSlot);
        }

        if (!found) continue;

        items_[itemIndex].gridCell = cell;
        items_[itemIndex].gridSpan = span;
        items_[itemIndex].slot = SlotFromCell(gridPages_, cell);
        items_[itemIndex].selected = true;
        MarkGridArea(usedSlots, cell, span);
        searchSlot = items_[itemIndex].slot + 1;
        targetCell = cell;
        targetCell.column += span.columns;
        if (targetCell.column >= page->columns)
        {
            targetCell.column = 0;
            targetCell.row += span.rows;
        }
        changed = true;
    }

    if (changed)
    {
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

// ── Grid free functions ─────────────────────────────────────

inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId)
{
    for (auto& p : pages) if (p.id == pageId) return &p;
    return nullptr;
}

inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal)
{
    return index * ((horizontal ? page.cellWidth : page.cellHeight) +
                    (horizontal ? page.gapX      : page.gapY));
}

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

inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell)
{
    auto* page = FindGridPage(pages, cell.pageId);
    int rows = page ? page->rows : 1;
    return std::max(0, cell.column) * std::max(1, rows) + std::max(0, cell.row);
}

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
            widget.gridSpan.columns = std::clamp(widget.gridSpan.columns, 1, std::max(1, page->columns));
            widget.gridSpan.rows    = std::clamp(widget.gridSpan.rows,    1, std::max(1, page->rows));
            widget.gridCell.column  = std::clamp(widget.gridCell.column,  0, std::max(0, page->columns - widget.gridSpan.columns));
            widget.gridCell.row     = std::clamp(widget.gridCell.row,     0, std::max(0, page->rows    - widget.gridSpan.rows));
        }
        widget.bounds = GetGridRect(gridPages_, widget.gridCell, widget.gridSpan);
    }
}

inline void DesktopApp::RebuildContainersAndItems()
{
    containers_.clear();
    items_oo_.clear();

    // Collect keys of items that belong to widgets
    std::unordered_set<std::wstring> collectedKeys;
    for (auto& w : widgets_)
        for (auto& k : w.itemKeys)
            collectedKeys.insert(k);

    // DesktopGrid
    auto grid = std::make_unique<DesktopGrid>(&gridPages_, &items_, this);
    containers_.push_back(std::move(grid));

    // DesktopIcon for each NON-collected DesktopItem
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (collectedKeys.contains(item.layoutKey)) continue;
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
}

inline void DesktopApp::EnumerateFolderMappingEntries(DesktopWidget& widget)
{
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
        widget.folderEntries.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

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
    RebuildContainersAndItems();
}

// ── Widget creation helpers ──────────────────────────────────

inline std::wstring DesktopApp::MakeNewWidgetId() const
{
    return L"widget-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
}

inline void DesktopApp::AddWidgetToGrid(DesktopWidget&& widget, GridSpan span)
{
    POINT clientPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &clientPoint);
    GridCell cell = CellFromPoint(clientPoint);
    if (cell.pageId.empty())
    {
        for (const auto& p : gridPages_) { cell = { p.id, 0, 0 }; break; }
        if (cell.pageId.empty()) return;
    }
    widget.gridCell = cell;
    widget.gridSpan = span;
    widget.showTitle = true;
    widgets_.push_back(std::move(widget));
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::AddCollectionWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::Collection;
    w.title = L"集合";
    w.bottomBarHover = true;
    AddWidgetToGrid(std::move(w), { 1, 1 });
}

inline void DesktopApp::AddFileCategoryWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::FileCategories;
    w.title = L"桌面文件";
    AddWidgetToGrid(std::move(w), { 2, 2 });
}

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
    w.sourceFolderPath = folderPath;
    AddWidgetToGrid(std::move(w), { 2, 2 });

    // Enumerate entries
    size_t idx = widgets_.size() - 1;
    EnumerateFolderMappingEntries(widgets_[idx]);
    RebuildContainersAndItems();
}

inline void DesktopApp::PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan)
{
    if (widgetIndex >= widgets_.size()) return;
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return;

    targetSpan.columns = std::clamp(targetSpan.columns, 1, std::max(1, page->columns - targetCell.column));
    targetSpan.rows    = std::clamp(targetSpan.rows,    1, std::max(1, page->rows    - targetCell.row));

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

    // Collect items displaced by this placement
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

    // Build occupied slot set (all widgets + non-displaced items)
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex) continue;
        MarkGridArea(usedSlots, widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        bool isDisplaced = std::find(displaced.begin(), displaced.end(), i) != displaced.end();
        if (!isDisplaced)
            MarkGridArea(usedSlots, items_[i].gridCell, items_[i].gridSpan);
    }
    MarkGridArea(usedSlots, targetCell, targetSpan);

    // Sort displaced items by grid order for deterministic placement
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

    // Re-home displaced items
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

    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
}

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