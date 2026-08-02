#include "app.h"

// Explorer change tracking, desktop reload and asynchronous icon completion.

void DesktopApp::RegisterShellChangeNotifications()
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
std::wstring DesktopApp::GetStableLayoutKey(
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
void DesktopApp::ApplyShortcutArrowToBitmap(HBITMAP bitmap, SIZE bitmapSize)
{
    if (!bitmap) return;
    SHSTOCKICONINFO sii{};
    sii.cbSize = sizeof(sii);
    if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) || !sii.hIcon)
        return;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(hdc, bitmap));
    int arrowSz = static_cast<int>(bitmapSize.cy * 30.0 / 64.0 + 0.5);
    if (arrowSz < 10) arrowSz = 10;
    int arrowX = static_cast<int>(bitmapSize.cx * 5.0 / 64.0 + 0.5);
    int arrowY = bitmapSize.cy - arrowSz;
    DrawIconEx(hdc, arrowX, arrowY, sii.hIcon, arrowSz, arrowSz, 0, nullptr, DI_NORMAL);
    SelectObject(hdc, oldBmp);
    DeleteDC(hdc);
    DestroyIcon(sii.hIcon);
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
LRESULT CALLBACK DesktopApp::ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
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
LRESULT DesktopApp::HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (systemTaskbarTaskViewStateMsg_ &&
        msg == systemTaskbarTaskViewStateMsg_)
    {
        const bool visible = wp != 0;
        if (systemTaskbarTaskViewActive_ != visible)
        {
            systemTaskbarTaskViewActive_ = visible;
            systemTaskbarWindowStateChangedTick_.fetch_add(1,
                std::memory_order_relaxed);
        }
        return 0;
    }
    if (taskbarRestartMsg_ && msg == taskbarRestartMsg_)
    {
        NotifySystemTaskbarCreated();
        systemTaskbarBackdropRefreshTick_ = 0;
        systemTaskbarTaskViewActive_ = false;
        systemTaskbarWindows_.clear();
        RestartSystemTaskbarShellVisibilityDetectors();
        systemTaskbarWindowStateChangedTick_.fetch_add(1,
            std::memory_order_relaxed);
        DWORD currentExplorerProcessId = 0;
        if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr))
            GetWindowThreadProcessId(taskbar, &currentExplorerProcessId);
        // Explorer can broadcast TaskbarCreated more than once while its shell
        // windows settle. Rebuild the desktop pipeline once per Explorer PID.
        if (!currentExplorerProcessId ||
            currentExplorerProcessId != desktopHostExplorerProcessId_)
            explorerDesktopRecreatePending_ = true;
        RecoverDesktopHostAfterExplorerRestart();
        return 0;
    }
    switch (msg)
    {
    case kFloatingDockBackdropCommitMessage:
        FinalizeFloatingDockBackdropCleanup(
            static_cast<UINT_PTR>(wp));
        return 0;
    case kForegroundInteractionChangedMessage:
        ReconcileDesktopHoverState();
        return 0;
    case kShellFileOperationCompletedMessage:
        OnShellFileOperationCompleted(lp);
        return 0;
    case kActivateExistingInstanceMessage:
        ShowSettingsWindow();
        return 0;
    case WM_DISPLAYCHANGE:
        ScheduleDisplayTopologyRefresh();
        return 0;
    case WM_DEVICECHANGE:
        switch (wp)
        {
        case DBT_DEVNODES_CHANGED:
        case DBT_CONFIGCHANGED:
        case DBT_DEVICEARRIVAL:
        case DBT_DEVICEREMOVECOMPLETE:
            ScheduleDisplayTopologyRefresh();
            break;
        default:
            break;
        }
        return TRUE;
    case kTrayCallbackMessage:
        OnTrayCallback(lp);
        return 0;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case WM_HOTKEY:
        if (settingsWindow_ &&
            settingsWindow_->IsHotkeyCaptureActive())
        {
            settingsWindow_->CaptureRegisteredHotkey(
                LOWORD(lp), HIWORD(lp));
            return 0;
        }
        if (static_cast<int>(wp) == kQuickNavigationHotkeyId)
        {
            ToggleQuickNavigation();
            return 0;
        }
        if (static_cast<int>(wp) ==
            kFloatingDockHotkeyId)
        {
            ToggleFloatingDock();
            return 0;
        }
        if (static_cast<int>(wp) ==
            kDesktopPassthroughHotkeyId)
        {
            BeginDesktopPassthroughHold();
            return 0;
        }
        break;
    case WM_COMMAND:
        return 0;
    case WM_CLOSE:
        RequestExit();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kDisplayTopologyRefreshTimerId);
        if (floatingDockHotkeyHwnd_ == hwnd)
        {
            floatingDockHotkeyHwnd_ = nullptr;
            floatingDockHotkeyRegistered_ = false;
        }
        if (desktopPassthroughHotkeyHwnd_ == hwnd)
        {
            KillTimer(hwnd, kDesktopPassthroughHoldTimerId);
            desktopPassthroughHotkeyHwnd_ = nullptr;
            desktopPassthroughHotkeyRegistered_ = false;
            desktopPassthroughHoldActive_ = false;
        }
        if (floatingDockEdgeSwipeHwnd_ == hwnd)
        {
            floatingDockEdgeSwipeHwnd_ = nullptr;
            floatingDockEdgeSwipeDetector_.Reset();
        }
        controlHwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 重新加载桌面项，可选择是否重新从磁盘读取布局。
 * @param reloadLayoutFromDisk 是否重新加载布局文件。
 */
void DesktopApp::ReloadItems(bool reloadLayoutFromDisk)
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    if (reloading_) return;
    reloading_ = true;
    dockAppIdentityCache_.clear();
    dockRunningWindows_.clear();
    dockFolderTargetCache_.clear();
    dockFolderIconIndexCache_.clear();
    BeginIconLoadGeneration();
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    if (reloadLayoutFromDisk)
    {
        LoadLayoutSlots();
        // The file has just populated savedPageColumns_/savedPageRows_. Do not
        // overwrite those restored values with the pre-reload runtime grid.
        UpdateLayoutWorkArea(false);
        if (widgetEngine_)
            widgetEngine_->ReloadStorage();
    }
    else
    {
        for (auto& widget : widgets_)
        {
            if (widget.type == DesktopWidgetType::FolderMapping)
                EnumerateFolderMappingEntries(widget);
        }
    }
    LoadDesktopItems();
    // LoadLayoutSlots may normalize Dock entries before the freshly
    // enumerated desktop items are available. Discard those provisional
    // resolutions so paths and shortcut targets are classified from the new
    // item snapshot.
    dockFolderTargetCache_.clear();
    dockFolderIconIndexCache_.clear();
    // A Shell delete removes the desktop item, but its persisted Dock mapping
    // otherwise survives and still consumes a slot.  Only prune references
    // that are confirmed missing on disk: hidden files and temporarily
    // unenumerated Shell items must remain pinned.
    std::erase_if(dockEntries_, [this](const DockEntry& entry) {
        if (entry.type != DockEntryType::DesktopItem)
            return false;
        if (IsRecycleBinDockEntry(entry))
            return FindItemIndexByKey(entry.reference) == static_cast<size_t>(-1);

        const std::wstring& path = entry.reference;
        const bool driveAbsolute = path.size() >= 3 &&
            ((path[0] >= L'A' && path[0] <= L'Z') ||
             (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
        const bool uncAbsolute = path.starts_with(L"\\\\");
        if (!driveAbsolute && !uncAbsolute)
            return false;

        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return false;
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_INVALID_NAME;
    });
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    if (!generalSettings_.dockEnabled && !dockEntries_.empty())
        RestoreDockEntriesToDesktop();
    ApplyAutoCollectFileCategoryWidgets();

    // Mark widgets as used
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    // Mark items with valid existing positions as used; flag unslotted items
    std::unordered_set<std::wstring> placedKeys;
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;

        auto* page = item.gridCell.pageId.empty()
            ? nullptr
            : FindGridPage(gridPages_, item.gridCell.pageId);
        if (page == nullptr)
        {
            // Item belongs to a page not currently visible — mark its slots as used
            const std::wstring& pid = item.gridCell.pageId;
            if (!pid.empty() && savedPageColumns_.count(pid) && savedPageRows_.count(pid))
            {
                int cols = savedPageColumns_[pid];
                int rows = savedPageRows_[pid];
                if (item.gridCell.column >= 0 && item.gridCell.row >= 0 &&
                    item.gridCell.column + item.gridSpan.columns <= cols &&
                    item.gridCell.row + item.gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan) &&
                    !placedKeys.contains(item.layoutKey))
                {
                    MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                    placedKeys.insert(item.layoutKey);
                }
            }
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

    // Track newly created virtual pages for overflow items
    std::unordered_map<std::wstring, int> overflowSlots;
    // Build quick-lookup of page IDs currently visible in gridPages_
    std::unordered_set<std::wstring> visiblePageIds2;
    for (const auto& gp : gridPages_)
        visiblePageIds2.insert(gp.id);

    for (auto* item : unslotted)
    {
        GridCell freeCell;
        if (TryFindFreeCell(item->gridSpan, usedSlots, freeCell))
        {
            item->gridCell = freeCell;
            MarkGridArea(usedSlots, freeCell, item->gridSpan);
            continue;
        }

        // Search all saved pages that aren't currently visible
        bool placedInSavedPage = false;
        for (const auto& pageId : savedPageIds_)
        {
            if (visiblePageIds2.count(pageId)) continue;
            if (!savedPageColumns_.count(pageId) || !savedPageRows_.count(pageId)) continue;
            int cols = savedPageColumns_[pageId];
            int rows = savedPageRows_[pageId];
            int capacity = std::max(1, cols * rows);
            for (int slot = 0; slot < capacity; ++slot)
            {
                GridCell candidate;
                candidate.pageId = pageId;
                candidate.column = slot / std::max(1, rows);
                candidate.row    = slot % std::max(1, rows);
                if (candidate.column + item->gridSpan.columns <= cols &&
                    candidate.row + item->gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, candidate, item->gridSpan))
                {
                    item->gridCell = candidate;
                    MarkGridArea(usedSlots, candidate, item->gridSpan);
                    placedInSavedPage = true;
                    break;
                }
            }
            if (placedInSavedPage) break;
        }
        if (placedInSavedPage) continue;

        // Try previously-created overflow pages
        bool placedInNewPage = false;
        for (auto& [pageId, nextSlot] : overflowSlots)
        {
            int cols = savedPageColumns_.count(pageId) ? savedPageColumns_[pageId] : 1;
            int rows = savedPageRows_.count(pageId) ? savedPageRows_[pageId] : 1;
            int capacity = std::max(1, cols * rows);
            for (int slot = nextSlot; slot < capacity; ++slot)
            {
                GridCell candidate;
                candidate.pageId = pageId;
                candidate.column = slot / std::max(1, rows);
                candidate.row    = slot % std::max(1, rows);
                if (candidate.column + item->gridSpan.columns <= cols &&
                    candidate.row + item->gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, candidate, item->gridSpan))
                {
                    item->gridCell = candidate;
                    MarkGridArea(usedSlots, candidate, item->gridSpan);
                    nextSlot = slot + 1;
                    placedInNewPage = true;
                    break;
                }
            }
            if (placedInNewPage) break;
        }
        if (placedInNewPage) continue;

        // No space anywhere — create a new virtual page on the last monitor
        if (!gridPages_.empty())
        {
            std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
            GridPage& lastPage = gridPages_[monitorOrder.back()];

            std::wstring newPageId = GeneratePageId();
            RememberSavedPageId(newPageId);
            savedPageColumns_[newPageId] = lastPage.columns;
            savedPageRows_[newPageId]    = lastPage.rows;

            item->gridCell.pageId = newPageId;
            item->gridCell.column = 0;
            item->gridCell.row    = 0;
            MarkGridArea(usedSlots, item->gridCell, item->gridSpan);
            overflowSlots[newPageId] = 1;
        }
    }

    // Loading new files may add virtual overflow pages, while deleting files
    // may remove the last usable offset. Refresh the runtime page mapping in
    // this same reload pass instead of waiting for the next manual refresh.
    ApplyPageMapping();
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
    RefreshDockRunningWindows(false);
    if (widgetEngine_)
        widgetEngine_->NotifyDesktopChanged("reload");
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::EnqueueIconLoad(IconLoadTask task)
{
    if (task.requestKey.empty())
    {
        const std::wstring& identity = task.isDesktopItem ? task.layoutKey : task.folderPath;
        task.requestKey = std::to_wstring(task.serial) + L"\n" +
            (task.isDesktopItem ? L"D\n" : L"F\n") + task.widgetId + L"\n" +
            ToUpperInvariant(identity) + L"\n" +
            (task.phase == IconLoadPhase::Phase1 ? L"1" : L"2");
    }
    {
        std::lock_guard<std::mutex> lock(iconLoaderMutex_);
        if (!iconLoaderPendingKeys_.insert(task.requestKey).second)
            return;
        iconLoaderQueue_.push_back(std::move(task));
    }
    iconLoaderCv_.notify_one();
}

void DesktopApp::OnIconLoaded(WPARAM /*wParam*/, LPARAM lParam)
{
    auto* result = reinterpret_cast<IconLoadResult*>(lParam);
    if (!result) return;

    std::unique_ptr<IconLoadResult> resultGuard(result);
    {
        std::lock_guard<std::mutex> lock(iconLoaderMutex_);
        iconLoaderPendingKeys_.erase(result->requestKey);
    }
    if (result->serial != iconLoadSerial_)
    {
        if (result->bitmap) DeleteObject(result->bitmap);
        result->bitmap = nullptr;
        return;
    }
    bool matched = false;

    if (result->isDesktopItem)
    {
        for (auto& item : items_)
        {
            if (ToUpperInvariant(item.layoutKey) == ToUpperInvariant(result->layoutKey))
            {
                if (result->bitmap)
                {
                    if (item.iconBitmap) { EraseD2DIconCacheForBitmap(item.iconBitmap); DeleteObject(item.iconBitmap); }
                    item.iconBitmap = result->bitmap;
                    item.iconBitmapSize = result->bitmapSize;
                    result->bitmap = nullptr;
                }
                matched = true;
                if (result->phase == IconLoadPhase::Phase1)
                {
                    item.iconState = IconState::IconReady;
                    item.shortcutArrow = result->shortcutArrow;
                    item.isShortcut = result->isShortcut;
                    item.isApplicationShortcut = result->isApplicationShortcut;
                    IconLoadTask phase2;
                    phase2.serial = result->serial;
                    phase2.layoutKey = item.layoutKey;
                    phase2.absolutePidl.reset(ILClone(item.absolutePidl.get()));
                    phase2.sysIconIndex = item.sysIconIndex;
                    phase2.parsingName = item.parsingName;
                    phase2.isDesktopItem = true;
                    phase2.phase = IconLoadPhase::Phase2;
                    EnqueueIconLoad(std::move(phase2));
                }
                else
                {
                    item.iconState = IconState::FullQuality;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (quickNavigationOpen_)
                    InvalidateQuickNavigationWindow();
                break;
            }
        }
    }
    else
    {
        for (auto& widget : widgets_)
        {
            if (widget.id != result->widgetId || widget.sourceFolderPath.empty()) continue;
            for (auto& entry : widget.folderEntries)
            {
                if (ToUpperInvariant(entry.fullPath) == ToUpperInvariant(result->folderPath))
                {
                    if (result->bitmap)
                    {
                        if (entry.iconBitmap) { EraseD2DIconCacheForBitmap(entry.iconBitmap); DeleteObject(entry.iconBitmap); }
                        entry.iconBitmap = result->bitmap;
                        entry.iconBitmapSize = result->bitmapSize;
                        result->bitmap = nullptr;
                    }
                    matched = true;
                    if (result->phase == IconLoadPhase::Phase1)
                    {
                        entry.iconState = IconState::IconReady;
                        entry.shortcutArrow = result->shortcutArrow;
                        entry.isShortcut = result->isShortcut;
                        entry.isApplicationShortcut = result->isApplicationShortcut;
                        IconLoadTask phase2;
                        phase2.serial = result->serial;
                        phase2.widgetId = widget.id;
                        phase2.folderPath = entry.fullPath;
                        phase2.sysIconIndex = entry.sysIconIndex;
                        phase2.isDesktopItem = false;
                        phase2.phase = IconLoadPhase::Phase2;
                        PIDLIST_ABSOLUTE pidl = nullptr;
                        if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                        {
                            phase2.absolutePidl.reset(pidl);
                            EnqueueIconLoad(std::move(phase2));
                        }
                    }
                    else
                    {
                        entry.iconState = IconState::FullQuality;
                    }
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (quickNavigationOpen_)
                        InvalidateQuickNavigationWindow();
                    break;
                }
            }
            break;
        }
    }

    if (!matched && result->bitmap)
        DeleteObject(result->bitmap);
}

/**
 * @brief 枚举桌面文件夹中的所有项，构建 DesktopItem 列表，包含图标、布局键和网格位置。
 *
 * 会依据 Windows“隐藏的项目”设置过滤隐藏项，同时过滤非桌面路径项，
 * 并为 .lnk 文件检测快捷方式箭头。
 */
