#include "app.h"
#include "shell_icon_request.h"
#include "startup_diagnostics.h"
#include "../shell_call_diagnostics.h"

#include "../desktop_namespace_registry.h"

namespace shellCalls = snowdesktop::shell_call_diagnostics;

// Shell desktop enumeration and display-topology refresh.

static bool ReadDesktopSource(
    const std::unordered_map<std::wstring, bool>& visibility,
    bool showHiddenItems, std::vector<DesktopItem>& items,
    snowdesktop::shell_refresh::MetadataCache* cache, bool simulated,
    const std::function<void(const DesktopItem&)>& publish = {},
    const std::wstring& directory = {})
{
    using namespace snowdesktop::shell_refresh;
    shellCalls::Context trace(directory.empty() ? L"desktop.full" : L"desktop.local", directory);
    ComPtr<IShellFolder> desktopFolder;
    HRESULT hr = shellCalls::Call(L"Setup.SHGetDesktopFolder", [&] {
        return SHGetDesktopFolder(&desktopFolder);
    });
    if (FAILED(hr) || !desktopFolder) return false;
    LPITEMIDLIST raw = nullptr;
    hr = shellCalls::Call(L"Setup.SHGetSpecialFolderLocation", [&] {
        return SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &raw);
    });
    if (FAILED(hr) || !raw) return false;
    Pidl desktopPidl(raw);
    const bool fileSystemSource = simulated || !directory.empty();
    const bool basicMetadata = !directory.empty();
    if (fileSystemSource)
    {
        PIDLIST_ABSOLUTE folderId = nullptr;
        const auto source = simulated ? snowdesktop::desktop_source::Directory() : directory;
        hr = shellCalls::Call(
            L"Setup.SHParseDisplayName",
            [&] {
                return SHParseDisplayName(source.c_str(), nullptr, &folderId, 0, nullptr);
            },
            source);
        if (FAILED(hr)) return false;
        desktopPidl.reset(folderId);
        ComPtr<IShellFolder> folder;
        hr = shellCalls::Call(L"Setup.BindToObject", [&] {
            return desktopFolder->BindToObject(folderId, nullptr, IID_PPV_ARGS(&folder));
        });
        if (FAILED(hr)) return false;
        desktopFolder = std::move(folder);
    }

    wchar_t userDesktopPath[MAX_PATH]{};
    wchar_t commonDesktopPath[MAX_PATH]{};
    wchar_t userProfilePath[MAX_PATH]{};
    shellCalls::Call(L"Setup.SpecialFolderPath.UserDesktop", [&] {
        return SHGetSpecialFolderPathW(nullptr, userDesktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
    });
    shellCalls::Call(L"Setup.SpecialFolderPath.CommonDesktop", [&] {
        return SHGetSpecialFolderPathW(nullptr, commonDesktopPath, CSIDL_COMMON_DESKTOPDIRECTORY,
                                       FALSE);
    });
    shellCalls::Call(L"Setup.SpecialFolderPath.Profile", [&] {
        return SHGetSpecialFolderPathW(nullptr, userProfilePath, CSIDL_PROFILE, FALSE);
    });
    size_t userDesktopLen = wcslen(userDesktopPath);
    size_t commonDesktopLen = wcslen(commonDesktopPath);
    const auto namespaceRegistrations =
        fileSystemSource ? std::vector<snowdesktop::DesktopNamespaceRegistration>{}
                         : shellCalls::Call(L"Setup.LoadNamespaceRegistrations", [&] {
                               return snowdesktop::LoadDesktopNamespaceRegistrations();
                           });

    SHCONTF enumFlags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
    if (showHiddenItems)
        enumFlags = static_cast<SHCONTF>(enumFlags | SHCONTF_INCLUDEHIDDEN);

    ComPtr<IEnumIDList> enumerator;
    hr = snowdesktop::startup_diagnostics::Call(L"Shell.EnumObjects", [&] {
        return shellCalls::Call(L"Enum.EnumObjects", [&] {
            return desktopFolder->EnumObjects(nullptr, enumFlags, &enumerator);
        });
    });
    if (FAILED(hr)) return false;
    if (!enumerator) return true;

    PITEMID_CHILD child = nullptr;
    ULONG fetched = 0;
    std::unordered_set<std::wstring> seenKeys;
    std::unordered_set<std::wstring> seenMetadata;
    HRESULT next = S_OK;
    for (size_t ordinal = 1;; ++ordinal)
    {
        trace.SetItem(ordinal);
        next = shellCalls::Call(L"Enum.Next", [&] {
            return enumerator->Next(1, &child, &fetched);
        });
        if (next != S_OK)
            break;
        PIDLIST_ABSOLUTE absolute = ILCombine(desktopPidl.get(), child);
        if (!absolute) { ILFree(child); continue; }

        // Get parsing name (used for CLSID detection)
        std::wstring parsingName = shellCalls::Call(L"Item.GetDisplayName.Parsing", [&] {
            return StrRetToString(desktopFolder.Get(), reinterpret_cast<PCUITEMID_CHILD>(child),
                                  SHGDN_FORPARSING);
        });

        // Get file system path
        wchar_t itemPath[MAX_PATH]{};
        std::wstring itemPathStr;
        if (shellCalls::Call(
                L"Item.SHGetPathFromIDList",
                [&] {
                    return SHGetPathFromIDListW(absolute, itemPath);
                },
                parsingName) &&
            itemPath[0])
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

        bool registeredNamespaceVisibleByDefault = false;
        std::wstring clsid = shellCalls::Call(
            L"Item.ResolveClsid",
            [&] {
                return ResolveDesktopIconClsid(parsingName, itemPathStr, userProfilePath);
            },
            parsingName);
        if (clsid.empty())
        {
            clsid = shellCalls::Call(
                L"Item.ResolveRegisteredClsid",
                [&] {
                    return snowdesktop::ResolveRegisteredDesktopNamespaceClsid(
                        absolute, itemPathStr, namespaceRegistrations,
                        &registeredNamespaceVisibleByDefault);
                },
                parsingName);
        }
        if (simulated) clsid.clear();
        bool isDesktopIcon = !clsid.empty();
        if (snowdesktop::debug_profile::Enabled() && !simulated && !isDesktopIcon)
        { ILFree(absolute); ILFree(child); continue; }

        // Standard desktop icons use the Explorer visibility registry. Other
        // entries, including third-party namespace aliases, still obey their
        // Shell hidden/non-enumerated attributes.
        WIN32_FILE_ATTRIBUTE_DATA fileAttributes{};
        const bool hasAttributes =
            !parsingName.empty() &&
            shellCalls::Call(
                L"Item.GetFileAttributesEx",
                [&] {
                    return GetFileAttributesExW(parsingName.c_str(), GetFileExInfoStandard,
                                                &fileAttributes);
                },
                parsingName);
        if (basicMetadata && hasAttributes && !showHiddenItems &&
            (fileAttributes.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
        { ILFree(absolute); ILFree(child); continue; }
        if (!basicMetadata && !snowdesktop::IsStandardDesktopIconClsid(clsid))
        {
            SFGAOF attrs = SFGAO_HIDDEN | SFGAO_NONENUMERATED;
            LPCITEMIDLIST childConst = child;
            if (SUCCEEDED(shellCalls::Call(
                    L"Item.GetAttributesOf",
                    [&] {
                        return desktopFolder->GetAttributesOf(1, &childConst, &attrs);
                    },
                    parsingName)))
            {
                if ((attrs & SFGAO_NONENUMERATED) ||
                    (!showHiddenItems && (attrs & SFGAO_HIDDEN)))
                { ILFree(absolute); ILFree(child); continue; }
            }
        }

        // Check visibility (applies to all items)
        if (!IsVisibleByDesktopIconSettings(
                clsid, visibility,
                registeredNamespaceVisibleByDefault))
        { ILFree(absolute); ILFree(child); continue; }

        // Non-desktop-icon: must be physically on desktop
        if (!simulated && !isDesktopIcon && !itemPathStr.empty())
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

        // Enumerate membership every time, but do not repeatedly ask Shell
        // extensions to resolve unchanged shortcut icons and display metadata.
        const auto stamp = FileStamp::From(fileAttributes);
        const auto metadataKey = ToUpperInvariant(parsingName);
        const auto info = ReadDesktopMetadata(basicMetadata, parsingName,
            metadataKey, stamp, hasAttributes, cache, seenMetadata,
            [absolute, &parsingName](SHFILEINFOW& value) {
                const auto started = GetTickCount64();
                const bool success =
                    shellCalls::Call(
                        L"Item.SHGetFileInfo.Metadata",
                        [&] {
                            return SHGetFileInfoW(reinterpret_cast<LPCWSTR>(absolute), 0, &value,
                                                  sizeof(value),
                                                  SHGFI_PIDL | SHGFI_SYSICONINDEX |
                                                      SHGFI_DISPLAYNAME | SHGFI_TYPENAME);
                        },
                        parsingName) != 0;
                const auto elapsed = GetTickCount64() - started;
                if (elapsed >= 250)
                {
                    const auto message = L"Shell desktop metadata slow: elapsed_ms=" +
                        std::to_wstring(elapsed) + L" path=" + parsingName;
                    WriteDiagnosticLogEntry(message.c_str());
                }
                return success;
            });

        DesktopItem item;
        item.absolutePidl.reset(absolute);
        item.childPidl.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(child));
        item.parsingName = std::move(parsingName);
        item.desktopIconClsid = std::move(clsid);
        item.name = basicMetadata ? LocalDesktopDisplayName(item.parsingName,
                        hasAttributes && (fileAttributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    : info.szDisplayName[0]
                        ? info.szDisplayName
                        : shellCalls::Call(
                              L"Item.GetDisplayName.Normal",
                              [&] {
                                  return StrRetToString(
                                      desktopFolder.Get(),
                                      reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()),
                                      SHGDN_NORMAL);
                              },
                              item.parsingName);
        item.typeName = info.szTypeName;
        if (hasAttributes)
        {
            item.modifiedTime = fileAttributes.ftLastWriteTime;
            if ((fileAttributes.dwFileAttributes &
                    FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                item.fileSize =
                    (static_cast<std::uint64_t>(
                        fileAttributes.nFileSizeHigh) << 32) |
                    static_cast<std::uint64_t>(
                        fileAttributes.nFileSizeLow);
            }
        }
        item.sysIconIndex = info.iIcon;
        item.layoutKey = ToUpperInvariant(!item.desktopIconClsid.empty()
            ? item.desktopIconClsid : !itemPathStr.empty()
                ? itemPathStr : item.parsingName);

        if (fileSystemSource)
            item.childPidl.reset(ILCloneFull(item.absolutePidl.get()));

        if (!seenKeys.insert(item.layoutKey).second)
            continue; // The local item owns both PIDLs, including duplicates.
        if (publish)
            shellCalls::Call(
                L"Item.Publish",
                [&] {
                    publish(item);
                },
                item.parsingName);
        items.push_back(std::move(item));
    }
    if (cache && SUCCEEDED(next))
        std::erase_if(cache->desktop, [&](const auto& entry) {
            return !seenMetadata.contains(entry.first);
        });
    return SUCCEEDED(next);
}

bool snowdesktop::shell_refresh::ReadDesktop(
    const std::unordered_map<std::wstring, bool>& visibility,
    bool showHiddenItems, std::vector<DesktopItem>& items, MetadataCache* cache,
    const std::function<void(const DesktopItem&)>& publish)
{
    snowdesktop::startup_diagnostics::Scope startup(
        L"Shell.Desktop.Read", static_cast<bool>(publish));
    if (!ReadDesktopSource(visibility, showHiddenItems, items, cache, false, publish)) return false;
    const bool complete = !snowdesktop::debug_profile::Enabled() ||
        ReadDesktopSource(visibility, showHiddenItems, items, nullptr, true, publish);
    startup.SetItems(items.size());
    return complete;
}

bool snowdesktop::shell_refresh::ReadLocalDesktop(
    const Request& request, Snapshot& snapshot, bool common)
{
    snowdesktop::startup_diagnostics::Scope startup(
        common ? L"Shell.LocalCommon.Read" : L"Shell.LocalUser.Read", true);
    shellCalls::Context trace(common ? L"desktop.local-common" : L"desktop.local-user", {});
    // Bypass the root Desktop enumerator: it can stall in NetUseEnum while
    // obtaining the next virtual/network item. Physical desktop folders are
    // independent sources, and publish each completed item immediately.
    const bool showHidden = shellCalls::Call(L"Setup.HiddenItemsVisible", [&] {
        return AreExplorerHiddenItemsVisible();
    });
    if (snowdesktop::debug_profile::Enabled())
    {
        return common || ReadDesktopSource(request.iconVisibility, showHidden,
            snapshot.desktopItems, nullptr, true, request.publishDesktopItem,
            snowdesktop::desktop_source::Directory());
    }
    wchar_t directory[MAX_PATH]{};
    if (!shellCalls::Call(L"Setup.LocalDesktopPath", [&] {
            return SHGetSpecialFolderPathW(
                nullptr, directory, common ? CSIDL_COMMON_DESKTOPDIRECTORY : CSIDL_DESKTOPDIRECTORY,
                FALSE);
        }))
        return false;
    const bool complete = ReadDesktopSource(request.iconVisibility, showHidden,
        snapshot.desktopItems, nullptr, false, request.publishDesktopItem, directory);
    startup.SetItems(snapshot.desktopItems.size());
    return complete;
}

void DesktopApp::LoadDesktopItems(snowdesktop::shell_refresh::Snapshot* snapshot)
{
    if (snapshot && !snapshot->desktopComplete && !snapshot->desktopIncremental)
        return; // Partial/failed reads must never become an authoritative empty model.
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    // Menu COM interfaces belong to the UI STA and are never shared with reads.
    if (!desktopFolder_ && FAILED(SHGetDesktopFolder(&desktopFolder_)))
        return;
    if (!desktopPidl_.get())
    {
        LPITEMIDLIST raw = nullptr;
        if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &raw)))
            return;
        desktopPidl_.reset(raw);
    }
    if (!snapshot) { RequestShellRefresh(); return; }
    auto fresh = std::move(snapshot->desktopItems);
    auto previous = std::exchange(items_, std::move(fresh));
    std::unordered_map<std::wstring, size_t> previousByKey;
    for (size_t i = 0; i < previous.size(); ++i)
        previousByKey.emplace(ToUpperInvariant(previous[i].layoutKey), i);
    itemIndexByKeyCache_.clear();
    itemTextLayoutCache_.clear();
    itemTextShadowCache_.clear();
    componentListTextLayoutCache_.clear();
    componentListTextShadowCache_.clear();
    for (auto& item : items_)
    {
        const auto found = previousByKey.find(ToUpperInvariant(item.layoutKey));
        if (found != previousByKey.end())
            snowdesktop::shell_refresh::PreserveRuntime(item, previous[found->second]);
        if (!snapshot || found == previousByKey.end())
        {
            item.gridCell = {};
            item.gridSpan = {1, 1};
            item.largeIcon.reset();
            item.slot = -1;
            const auto known = layoutRecords_.find(item.layoutKey);
            if (known != layoutRecords_.end() && known->second.hasGrid)
            {
                item.gridCell = known->second.cell;
                item.gridSpan = known->second.span;
                item.largeIcon = known->second.largeIcon;
                item.slot = SlotFromCell(gridPages_, item.gridCell);
            }
        }
        // Explicit refresh/settings changes must still rebuild icons even if
        // Shell reuses an image-list index and the file timestamps are equal.
        if (!snapshot)
            item.iconState = IconState::Loading;
        if (!item.iconBitmap || item.iconState != IconState::FullQuality)
        {
            IconLoadTask task;
            task.sourceStamp = snowdesktop::shell_icon_request::Stamp(item);
            task.serial = iconLoadSerial_;
            task.layoutKey = item.layoutKey;
            task.absolutePidl.reset(ILCloneFull(item.absolutePidl.get()));
            task.sysIconIndex = item.sysIconIndex;
            task.parsingName = item.parsingName;
            task.isDesktopItem = true;
            task.phase = item.iconState == IconState::IconReady
                ? IconLoadPhase::Phase2 : IconLoadPhase::Phase1;
            EnqueueIconLoad(std::move(task));
        }
    }
    if (snapshot && snapshot->desktopIncremental)
        snowdesktop::shell_refresh::AppendUnobservedItems(items_, previous);
    for (const auto& oldItem : previous)
        if (oldItem.iconBitmap)
            EraseD2DIconCacheForBitmap(oldItem.iconBitmap);
    RefreshDesktopItemIndexCache();
    desktopItemsReady_ = !snapshot || snapshot->desktopComplete;
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
 * @brief 判断当前桌面窗口是否仍完整覆盖虚拟屏幕。
 *
 * 某些显卡/扩展坞路径不会向隐藏控制窗口投递显示变化通知。即使显示器
 * 枚举结果已经稳定，Explorer 下的子窗口也可能仍保持旧屏幕尺寸。
 */
bool DesktopApp::DesktopWindowNeedsDisplaySynchronization() const
{
    if (!customDesktopVisible_ || !hwnd_ || !IsWindow(hwnd_))
        return false;

    RECT windowRect{};
    if (!GetWindowRect(hwnd_, &windowRect))
        return true;

    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 0 || virtualHeight <= 0)
        return false;

    return windowRect.left != virtualLeft ||
        windowRect.top != virtualTop ||
        windowRect.right != virtualLeft + virtualWidth ||
        windowRect.bottom != virtualTop + virtualHeight;
}

/**
 * @brief 低成本轮询显示器拓扑和桌面窗口覆盖范围。
 */
void DesktopApp::PollDisplayTopology()
{
    if (exitRequested_)
        return;

    if (gridPages_.empty() ||
        CaptureDisplayTopologySignature() != displayTopologySignature_ ||
        DesktopWindowNeedsDisplaySynchronization())
    {
        ScheduleDisplayTopologyRefresh();
    }
}

/**
 * @brief 在显示器拓扑实际变化后调整桌面覆盖层并重建布局。
 */
void DesktopApp::RefreshDisplayTopologyIfChanged()
{
    if (exitRequested_ || desktopStartupPresentationPending_)
        return;

    const std::wstring currentSignature = CaptureDisplayTopologySignature();
    const bool topologyChanged = gridPages_.empty() ||
        currentSignature != displayTopologySignature_;
    const bool windowBoundsOutOfSync =
        DesktopWindowNeedsDisplaySynchronization();
    const auto refreshAction =
        snowdesktop::display_topology_refresh::ResolveAction(
            topologyChanged,
            displayTopologyWindowSyncPending_,
            windowBoundsOutOfSync);
    if (refreshAction ==
        snowdesktop::display_topology_refresh::Action::None)
        return;

    if (reloading_)
    {
        ScheduleDisplayTopologyRefresh();
        return;
    }

    // The escape edges belong to the previous monitor geometry. Leave the
    // temporary mode before rebuilding windows for a changed display layout.
    EndDesktopPassthrough();
    bool recreateExpandedOverlay = false;
    if (topologyChanged)
    {
        snowdesktop::display_topology_refresh::PageIdSet
            previousMappedPageIds;
        for (const auto& page : gridPages_)
        {
            if (!page.id.empty())
                previousMappedPageIds.insert(page.id);
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

        const snowdesktop::display_topology_refresh::Bounds previousBounds{
            virtualLeft_, virtualTop_,
            virtualLeft_ + virtualWidth_, virtualTop_ + virtualHeight_ };
        const snowdesktop::display_topology_refresh::Bounds currentBounds{
            newVirtualLeft, newVirtualTop,
            newVirtualLeft + newVirtualWidth,
            newVirtualTop + newVirtualHeight };
        recreateExpandedOverlay = customDesktopVisible_ &&
            hwnd_ && IsWindow(hwnd_) &&
            snowdesktop::display_topology_refresh::ExtendsBeyond(
                previousBounds, currentBounds);

        virtualLeft_ = newVirtualLeft;
        virtualTop_ = newVirtualTop;
        virtualWidth_ = newVirtualWidth;
        virtualHeight_ = newVirtualHeight;

        // Build the monitor pages before a replacement window performs its
        // first synchronous paint.
        if (!UpdateLayoutWorkArea())
        {
            // Keep the old coordinate origin paired with the retained pages.
            // The signature is not committed, so the next poll retries.
            virtualLeft_ = previousBounds.left;
            virtualTop_ = previousBounds.top;
            virtualWidth_ = previousBounds.right - previousBounds.left;
            virtualHeight_ = previousBounds.bottom - previousBounds.top;
            return;
        }
        LayoutItems();

        snowdesktop::display_topology_refresh::PageIdSet
            currentMappedPageIds;
        for (const auto& page : gridPages_)
        {
            if (!page.id.empty())
                currentMappedPageIds.insert(page.id);
        }
        displayTopologyHiddenPageIds_ =
            snowdesktop::display_topology_refresh::ReconcileHiddenPages(
                displayTopologyHiddenPageIds_,
                previousMappedPageIds, currentMappedPageIds);
    }

    if (recreateExpandedOverlay)
    {
        // Resizing the cross-process layered child updates GetWindowRect but,
        // on monitor hot-add, Windows can retain the old DirectComposition
        // input allocation. Recreate the HWND and its DComp target exactly as
        // startup does so the added pixels participate in hit testing.
        // Scheduler deadlines are independent of HWND lifetime. Retire the
        // old widget tokens before the common overlay-creation path creates
        // replacements for the recreated desktop host.
        for (const auto& [timerId, _] : widgetTimerIds_)
            uiAnimationScheduler_.Cancel(timerId);
        widgetTimerIds_.clear();
        const HWND previousWindow = hwnd_;
        DestroyWindow(previousWindow);
        if (!CreateDesktopOverlayWindow())
        {
            WriteDiagnosticLogEntry(
                L"Display topology overlay recreation failed",
                DiagnosticLogLevel::Error);
            ScheduleDisplayTopologyRefresh();
            return;
        }
        HideExplorerIcons();
        UpdateHostInputImePosition();
    }
    else if (hwnd_ && IsWindow(hwnd_))
    {
        HWND parent = GetParent(hwnd_);
        updatingDisplayTopology_ = true;
        if (customDesktopVisible_ && parent && IsWindow(parent))
        {
            // Reuse the complete startup attachment path. Besides resizing,
            // it reapplies the child frame, z-order, backdrop sibling and
            // keyboard-input sibling. A bare SetWindowPos can leave the newly
            // added part of a layered child window visible but outside
            // Explorer's effective mouse hit-test surface.
            AttachWindowToDesktopHost(parent);
            UpdateHostInputImePosition();
        }
        else
        {
            POINT origin{ virtualLeft_, virtualTop_ };
            if (parent && IsWindow(parent))
                ScreenToClient(parent, &origin);
            SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y,
                virtualWidth_, virtualHeight_,
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        updatingDisplayTopology_ = false;

        if (topologyChanged)
        {
            ResetDesktopWidgetComposition();
            dcompSurface_.Reset();
            compositionWidth_ = 0;
            compositionHeight_ = 0;
        }
    }

    if (topologyChanged)
    {
        uiAnimationScheduler_.RefreshDisplayRate();
        displayTopologySignature_ = currentSignature;

        // Monitor APIs can settle before Explorer has finished resizing and
        // reordering Progman/WorkerW. Run one more native-window-only pass
        // after the debounce interval even though the signature is then equal.
        displayTopologyWindowSyncPending_ = true;
        ScheduleDisplayTopologyRefresh();
    }
    else
    {
        displayTopologyWindowSyncPending_ = false;
    }

    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 更新布局工作区，枚举显示器并创建对应 GridPage，然后应用页面映射。
 */
