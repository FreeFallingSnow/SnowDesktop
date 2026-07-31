#include "app.h"

// Shell desktop enumeration and display-topology refresh.

void DesktopApp::LoadDesktopItems()
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);

    struct OldIcon {
        HBITMAP bitmap = nullptr;
        SIZE size{};
        int sysIconIndex = -1;
        bool shortcutArrow = false;
        bool isShortcut = false;
        bool isApplicationShortcut = false;
        IconState iconState = IconState::Loading;
    };
    std::unordered_map<std::wstring, OldIcon> oldIconCache;
    for (auto& item : items_) {
        if (!item.layoutKey.empty() && item.iconBitmap) {
            OldIcon old;
            old.bitmap = item.iconBitmap;
            old.size = item.iconBitmapSize;
            old.sysIconIndex = item.sysIconIndex;
            old.shortcutArrow = item.shortcutArrow;
            old.isShortcut = item.isShortcut;
            old.isApplicationShortcut = item.isApplicationShortcut;
            old.iconState = item.iconState;
            oldIconCache.emplace(ToUpperInvariant(item.layoutKey), std::move(old));
            item.iconBitmap = nullptr;
        }
    }
items_.clear();
    itemIndexByKeyCache_.clear();
    itemTextLayoutCache_.clear();
    itemTextShadowCache_.clear();
    WriteDiagnosticLogEntry(L"LoadItems start");

    HRESULT hr = SHGetDesktopFolder(&desktopFolder_);
    if (FAILED(hr) || !desktopFolder_) { WriteDiagnosticLogEntry(L"SHGetDesktopFolder FAILED"); return; }
    WriteDiagnosticLogEntry(L"DesktopFolder ok");

    LPITEMIDLIST raw = nullptr;
    hr = SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &raw);
    if (FAILED(hr) || !raw) { WriteDiagnosticLogEntry(L"SHGetSpecialFolderLocation FAILED"); return; }
    desktopPidl_.reset(raw);
    WriteDiagnosticLogEntry(L"DesktopPidl ok");

    wchar_t userDesktopPath[MAX_PATH]{};
    wchar_t commonDesktopPath[MAX_PATH]{};
    wchar_t userProfilePath[MAX_PATH]{};
    SHGetSpecialFolderPathW(nullptr, userDesktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
    SHGetSpecialFolderPathW(nullptr, commonDesktopPath, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
    SHGetSpecialFolderPathW(nullptr, userProfilePath, CSIDL_PROFILE, FALSE);
    size_t userDesktopLen = wcslen(userDesktopPath);
    size_t commonDesktopLen = wcslen(commonDesktopPath);

    const bool showHiddenItems = AreExplorerHiddenItemsVisible();
    SHCONTF enumFlags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
    if (showHiddenItems)
        enumFlags = static_cast<SHCONTF>(enumFlags | SHCONTF_INCLUDEHIDDEN);

    ComPtr<IEnumIDList> enumerator;
    hr = desktopFolder_->EnumObjects(hwnd_, enumFlags, &enumerator);
    if (FAILED(hr) || !enumerator) { WriteDiagnosticLogEntry(L"EnumObjects FAILED"); return; }
    WriteDiagnosticLogEntry(L"EnumObjects ok");

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

        if (snowdesktop::shell_item_visibility::
                IsAlwaysHidden(
                    itemPathStr.empty()
                        ? parsingName
                        : itemPathStr))
        {
            ILFree(absolute);
            ILFree(child);
            continue;
        }

        std::wstring clsid = ResolveDesktopIconClsid(parsingName, itemPathStr, userProfilePath);
        bool isDesktopIcon = !clsid.empty();

        // Non-desktop-icon: always skip non-enumerated shell items, and follow
        // Explorer's "Hidden items" setting for ordinary hidden entries.
        if (!isDesktopIcon)
        {
            SFGAOF attrs = SFGAO_HIDDEN | SFGAO_NONENUMERATED;
            LPCITEMIDLIST childConst = child;
            if (SUCCEEDED(desktopFolder_->GetAttributesOf(1, &childConst, &attrs)))
            {
                if ((attrs & SFGAO_NONENUMERATED) ||
                    (!showHiddenItems && (attrs & SFGAO_HIDDEN)))
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
        item.layoutKey = GetStableLayoutKey(item.absolutePidl.get(), item.parsingName, item.desktopIconClsid);

auto oldIt = oldIconCache.find(ToUpperInvariant(item.layoutKey));
        if (oldIt != oldIconCache.end() && oldIt->second.sysIconIndex == item.sysIconIndex) {
            item.iconBitmap = oldIt->second.bitmap;
            item.iconBitmapSize = oldIt->second.size;
            item.shortcutArrow = oldIt->second.shortcutArrow;
            item.isShortcut = oldIt->second.isShortcut;
            item.isApplicationShortcut = oldIt->second.isApplicationShortcut;
            item.iconState = oldIt->second.iconState;
            oldIt->second.bitmap = nullptr;
            oldIconCache.erase(oldIt);
            if (item.iconState == IconState::IconReady)
            {
                IconLoadTask phase2;
                phase2.serial = iconLoadSerial_;
                phase2.layoutKey = item.layoutKey;
                phase2.absolutePidl.reset(ILClone(item.absolutePidl.get()));
                phase2.sysIconIndex = item.sysIconIndex;
                phase2.parsingName = item.parsingName;
                phase2.isDesktopItem = true;
                phase2.phase = IconLoadPhase::Phase2;
                EnqueueIconLoad(std::move(phase2));
            }
        } else {
            if (oldIt != oldIconCache.end()) {
                if (oldIt->second.bitmap) {
                    EraseD2DIconCacheForBitmap(oldIt->second.bitmap);
                    DeleteObject(oldIt->second.bitmap);
                }
                oldIconCache.erase(oldIt);
            }
            item.iconBitmap = nullptr;
            item.iconState = IconState::Loading;

            IconLoadTask task;
            task.serial = iconLoadSerial_;
            task.layoutKey = item.layoutKey;
            task.absolutePidl.reset(ILClone(item.absolutePidl.get()));
            task.sysIconIndex = item.sysIconIndex;
            task.parsingName = item.parsingName;
            task.isDesktopItem = true;
            task.phase = IconLoadPhase::Phase1;
            EnqueueIconLoad(std::move(task));
        }

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

    for (auto& [key, old] : oldIconCache) {
        if (old.bitmap) {
            EraseD2DIconCacheForBitmap(old.bitmap);
            DeleteObject(old.bitmap);
        }
    }

    wsprintfW(buf, L"Loaded %d items", count);
    WriteDiagnosticLogEntry(buf);
    RefreshDesktopItemIndexCache();
}

/**
 * @brief 捕获当前活动显示器拓扑的稳定签名。
 *
 * 签名包含虚拟桌面范围，以及每台活动显示器的设备名、屏幕范围、
 * 工作区、主屏标记和有效 DPI。排序后再拼接，避免枚举顺序变化导致误判。
 */
std::wstring DesktopApp::CaptureDisplayTopologySignature() const
{
    struct DisplayRecord
    {
        std::wstring deviceName;
        RECT monitor{};
        RECT work{};
        DWORD flags = 0;
        UINT dpiX = 0;
        UINT dpiY = 0;
    };

    std::vector<DisplayRecord> records;
    auto callback = [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        auto* output = reinterpret_cast<std::vector<DisplayRecord>*>(param);
        if (!output)
            return FALSE;

        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return TRUE;

        DisplayRecord record;
        record.deviceName = info.szDevice;
        record.monitor = info.rcMonitor;
        record.work = info.rcWork;
        record.flags = info.dwFlags;
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &record.dpiX, &record.dpiY);
        output->push_back(std::move(record));
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, callback, reinterpret_cast<LPARAM>(&records));

    std::sort(records.begin(), records.end(), [](const DisplayRecord& a, const DisplayRecord& b) {
        const int nameOrder = _wcsicmp(a.deviceName.c_str(), b.deviceName.c_str());
        if (nameOrder != 0) return nameOrder < 0;
        if (a.monitor.left != b.monitor.left) return a.monitor.left < b.monitor.left;
        if (a.monitor.top != b.monitor.top) return a.monitor.top < b.monitor.top;
        if (a.monitor.right != b.monitor.right) return a.monitor.right < b.monitor.right;
        return a.monitor.bottom < b.monitor.bottom;
    });

    std::wstring signature =
        std::to_wstring(GetSystemMetrics(SM_XVIRTUALSCREEN)) + L"," +
        std::to_wstring(GetSystemMetrics(SM_YVIRTUALSCREEN)) + L"," +
        std::to_wstring(GetSystemMetrics(SM_CXVIRTUALSCREEN)) + L"," +
        std::to_wstring(GetSystemMetrics(SM_CYVIRTUALSCREEN));

    for (const auto& record : records)
    {
        signature += L"|";
        signature += record.deviceName;
        signature += L":" + std::to_wstring(record.monitor.left);
        signature += L"," + std::to_wstring(record.monitor.top);
        signature += L"," + std::to_wstring(record.monitor.right);
        signature += L"," + std::to_wstring(record.monitor.bottom);
        signature += L":" + std::to_wstring(record.work.left);
        signature += L"," + std::to_wstring(record.work.top);
        signature += L"," + std::to_wstring(record.work.right);
        signature += L"," + std::to_wstring(record.work.bottom);
        signature += L":" + std::to_wstring(record.flags);
        signature += L":" + std::to_wstring(record.dpiX);
        signature += L"," + std::to_wstring(record.dpiY);
    }
    return signature;
}

/**
 * @brief 防抖调度显示器拓扑复查。
 *
 * 重复设备通知只会重置同一个一次性定时器，不会重复枚举或重建布局。
 */
void DesktopApp::ScheduleDisplayTopologyRefresh()
{
    HWND timerWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_
        : (hwnd_ && IsWindow(hwnd_) ? hwnd_ : nullptr);
    if (timerWindow)
        SetTimer(timerWindow, kDisplayTopologyRefreshTimerId,
            kDisplayTopologyRefreshDebounceMs, nullptr);
}

/**
 * @brief 在显示器拓扑实际变化后调整桌面覆盖层并重建布局。
 */
void DesktopApp::RefreshDisplayTopologyIfChanged()
{
    if (exitRequested_)
        return;

    const std::wstring currentSignature = CaptureDisplayTopologySignature();
    if (currentSignature == displayTopologySignature_)
        return;

    if (reloading_)
    {
        ScheduleDisplayTopologyRefresh();
        return;
    }

    const int newVirtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int newVirtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int newVirtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int newVirtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (newVirtualWidth <= 0 || newVirtualHeight <= 0)
    {
        ScheduleDisplayTopologyRefresh();
        return;
    }

    virtualLeft_ = newVirtualLeft;
    virtualTop_ = newVirtualTop;
    virtualWidth_ = newVirtualWidth;
    virtualHeight_ = newVirtualHeight;

    if (hwnd_ && IsWindow(hwnd_))
    {
        HWND parent = GetParent(hwnd_);
        POINT origin{ virtualLeft_, virtualTop_ };
        if (parent && IsWindow(parent))
            ScreenToClient(parent, &origin);

        updatingDisplayTopology_ = true;
        SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y,
            virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
        updatingDisplayTopology_ = false;

        dcompSurface_.Reset();
        compositionWidth_ = 0;
        compositionHeight_ = 0;
    }

    UpdateLayoutWorkArea();
    LayoutItems();
    displayTopologySignature_ = currentSignature;

    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 更新布局工作区，枚举显示器并创建对应 GridPage，然后应用页面映射。
 */
