#include "app.h"

// Folder-mapping enumeration and automatic file-category collection.

void DesktopApp::EnumerateFolderMappingEntries(
    DesktopWidget& widget, bool enqueueIconLoads)
{
    const bool showHiddenItems = AreExplorerHiddenItemsVisible();

    struct OldFolderIcon {
        HBITMAP bitmap = nullptr;
        SIZE size{};
        int sysIconIndex = -1;
        bool shortcutArrow = false;
        bool isShortcut = false;
        bool isApplicationShortcut = false;
        bool iconIsMediaThumbnail = false;
        IconState iconState = IconState::Loading;
    };
    std::unordered_map<std::wstring, OldFolderIcon> oldFolderIconCache;

    // Preserve the current entry state and bitmap across directory enumeration.
    for (auto& entry : widget.folderEntries) {
        if (!entry.fullPath.empty()) {
            OldFolderIcon old;
            old.bitmap = entry.iconBitmap;
            old.size = entry.iconBitmapSize;
            old.sysIconIndex = entry.sysIconIndex;
            old.shortcutArrow = entry.shortcutArrow;
            old.isShortcut = entry.isShortcut;
            old.isApplicationShortcut = entry.isApplicationShortcut;
            old.iconIsMediaThumbnail = entry.iconIsMediaThumbnail;
            old.iconState = entry.iconState;
            oldFolderIconCache.emplace(ToUpperInvariant(entry.fullPath), std::move(old));
            entry.iconBitmap = nullptr;
        } else if (entry.iconBitmap) {
            DeleteObject(entry.iconBitmap);
        }
    }

    widget.folderEntries.clear();
    if (widget.sourceFolderPath.empty()) {
        for (auto& [key, old] : oldFolderIconCache) {
            if (old.bitmap) {
                EraseD2DIconCacheForBitmap(old.bitmap);
                DeleteObject(old.bitmap);
            }
        }
        return;
    }
    std::wstring search = widget.sourceFolderPath + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        for (auto& [key, old] : oldFolderIconCache) {
            if (old.bitmap) {
                EraseD2DIconCacheForBitmap(old.bitmap);
                DeleteObject(old.bitmap);
            }
        }
        return;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (snowdesktop::shell_item_visibility::
                IsAlwaysHidden(fd.cFileName))
            continue;
        if (!showHiddenItems && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        FolderEntry entry;
        entry.name = fd.cFileName;
        entry.fullPath = widget.sourceFolderPath + L"\\" + fd.cFileName;
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.lastWriteTime = fd.ftLastWriteTime;
        if (!entry.isDirectory)
        {
            entry.fileSize =
                (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) |
                static_cast<std::uint64_t>(fd.nFileSizeLow);
        }
        SHFILEINFOW info{};
        SHGetFileInfoW(entry.fullPath.c_str(), 0, &info, sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_TYPENAME);
        entry.sysIconIndex = info.iIcon;
        entry.typeName = info.szTypeName;

        auto oldIt = oldFolderIconCache.find(ToUpperInvariant(entry.fullPath));
        if (oldIt != oldFolderIconCache.end() && oldIt->second.sysIconIndex == entry.sysIconIndex) {
            entry.iconBitmap = oldIt->second.bitmap;
            entry.iconBitmapSize = oldIt->second.size;
            entry.shortcutArrow = oldIt->second.shortcutArrow;
            entry.isShortcut = oldIt->second.isShortcut;
            entry.isApplicationShortcut = oldIt->second.isApplicationShortcut;
            entry.iconIsMediaThumbnail = oldIt->second.iconIsMediaThumbnail;
            entry.iconState = oldIt->second.iconState;
            oldIt->second.bitmap = nullptr;
            oldFolderIconCache.erase(oldIt);
            if (enqueueIconLoads &&
                entry.iconState == IconState::IconReady)
            {
                IconLoadTask phase2;
                phase2.serial = iconLoadSerial_;
                phase2.widgetId = widget.id;
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                {
                    phase2.absolutePidl.reset(pidl);
                    phase2.folderPath = entry.fullPath;
                    phase2.sysIconIndex = entry.sysIconIndex;
                    phase2.isDesktopItem = false;
                    phase2.phase = IconLoadPhase::Phase2;
                    EnqueueIconLoad(std::move(phase2));
                }
            }
            else if (enqueueIconLoads &&
                entry.iconState == IconState::Loading)
            {
                IconLoadTask phase1;
                phase1.serial = iconLoadSerial_;
                phase1.widgetId = widget.id;
                phase1.folderPath = entry.fullPath;
                phase1.sysIconIndex = entry.sysIconIndex;
                phase1.parsingName = entry.name;
                phase1.isDesktopItem = false;
                phase1.phase = IconLoadPhase::Phase1;
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                {
                    phase1.absolutePidl.reset(pidl);
                    EnqueueIconLoad(std::move(phase1));
                }
            }
        } else {
            if (oldIt != oldFolderIconCache.end()) {
                if (oldIt->second.bitmap) {
                    EraseD2DIconCacheForBitmap(oldIt->second.bitmap);
                    DeleteObject(oldIt->second.bitmap);
                }
                oldFolderIconCache.erase(oldIt);
            }
            entry.iconBitmap = nullptr;
            entry.iconState = IconState::Loading;

            if (enqueueIconLoads)
            {
                IconLoadTask task;
                task.serial = iconLoadSerial_;
                task.widgetId = widget.id;
                task.layoutKey = ToUpperInvariant(entry.fullPath);
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                {
                    task.absolutePidl.reset(pidl);
                    task.sysIconIndex = entry.sysIconIndex;
                    task.parsingName = entry.name;
                    task.isDesktopItem = false;
                    task.folderPath = entry.fullPath;
                    task.phase = IconLoadPhase::Phase1;
                    EnqueueIconLoad(std::move(task));
                }
            }
        }
        widget.folderEntries.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    for (auto& [key, old] : oldFolderIconCache) {
        if (old.bitmap) {
            EraseD2DIconCacheForBitmap(old.bitmap);
            DeleteObject(old.bitmap);
        }
    }
    std::sort(widget.folderEntries.begin(), widget.folderEntries.end(),
        [](const FolderEntry& a, const FolderEntry& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    if (snowdesktop::folder_sort_rules::
            NormalizeMode(widget.folderSortMode) !=
        snowdesktop::folder_sort_rules::kManual)
    {
        snowdesktop::folder_sort_rules::StableSort(
            widget.folderEntries,
            widget.folderSortMode,
            widget.folderSortAscending);
    }
    else if (!widget.itemKeys.empty())
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
    snowdesktop::folder_sort_rules::RewriteOrderKeys(
        widget.folderEntries, widget.itemKeys);
}

/**
 * @brief 刷新文件夹映射组件的内容（重新枚举目录）。
 * @param widgetIndex 组件索引。
 */
void DesktopApp::RefreshFolderMappingWidget(size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return;
    auto& w = widgets_[widgetIndex];
    EnumerateFolderMappingEntries(w);
    for (auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (wc && wc->GetWidgetData() == &w)
        {
            if (auto* mapping = dynamic_cast<FolderMapping*>(wc))
                mapping->InvalidateFilterCache();
            else
                wc->InvalidateSlots();
            break;
        }
    }
    // NOTE: caller must RebuildContainersAndItems + SaveLayoutSlots + InvalidateDesktop
}

/**
 * @brief 收集桌面文件到文件分类组件中。
 * @param widgetIndex 组件索引。
 * @param persist 是否立即持久化布局。
 * @return 有变化返回 true。
 */
bool DesktopApp::CollectFileCategoryWidget(size_t widgetIndex, bool persist)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FileCategories)
        return false;

    FileCategories collector(&widgets_[widgetIndex], this);
    bool changed = collector.CollectTopLevelDesktopItems();
    if (!changed) return false;
    RefreshCollectedKeysCache();

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
void DesktopApp::EnforceSingleAutoCollectFileCategory(size_t activeWidgetIndex)
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
void DesktopApp::ApplyAutoCollectFileCategoryWidgets()
{
    size_t active = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].type != DesktopWidgetType::FileCategories)
            continue;

        FileCategories collector(&widgets_[i], this);
        collector.PruneUncollectableItems();

        if (widgets_[i].autoCollect)
        {
            if (active == static_cast<size_t>(-1))
                active = i;
            else
                widgets_[i].autoCollect = false;
        }
    }
    RefreshCollectedKeysCache();
    if (active != static_cast<size_t>(-1))
        CollectFileCategoryWidget(active, false);
}

// ── 组件创建辅助函数 ──────────────────────────────────

/**
 * @brief 生成一个新的唯一组件 ID。
 * @return 组件 ID 字符串。
 */
