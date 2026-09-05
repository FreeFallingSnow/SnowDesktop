#include "app.h"
#include "../folder_mapping_rules.h"

// Folder-mapping enumeration and automatic file-category collection.

snowdesktop::shell_refresh::FolderSnapshot snowdesktop::shell_refresh::ReadFolder(
    const std::wstring& path, bool showHiddenItems)
{
    FolderSnapshot result;
    result.path = path;
    if (path.empty())
    {
        result.complete = true;
        return result;
    }
    // 磁盘根目录（如 "C:\"）自身以反斜杠结尾：直接拼接会生成 "C:\\名称"
    // 这种双反斜杠路径，SHParseDisplayName 对其返回 E_INVALIDARG，
    // 图标加载任务因此永远无法入队，条目只能停留在占位图标。
    // ChildPath 先剥离尾部分隔符，再用单一反斜杠拼接搜索串与条目路径。
    std::wstring search =
        snowdesktop::folder_mapping_rules::ChildPath(
            path, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        result.complete = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        return result;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (snowdesktop::shell_item_visibility::
                IsAlwaysHidden(fd.cFileName))
            continue;
        if (!showHiddenItems && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        FolderEntry entry;
        entry.name = fd.cFileName;
        entry.fullPath =
            snowdesktop::folder_mapping_rules::ChildPath(
                path, fd.cFileName);
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

        PIDLIST_ABSOLUTE absolute = nullptr;
        if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &absolute, 0, nullptr)))
            result.absoluteIds.emplace(ToUpperInvariant(entry.fullPath), Pidl(absolute));
        result.entries.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    result.complete = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(hFind);
    return result;
}

void DesktopApp::EnumerateFolderMappingEntries(DesktopWidget& widget,
    bool enqueueIconLoads, const snowdesktop::shell_refresh::FolderSnapshot* snapshot)
{
    if (snapshot && ToUpperInvariant(snapshot->path) !=
            ToUpperInvariant(widget.sourceFolderPath))
    {
        RequestShellRefresh();
        return;
    }
    snowdesktop::shell_refresh::FolderSnapshot local;
    if (!snapshot)
    {
        local = snowdesktop::shell_refresh::ReadFolder(widget.sourceFolderPath,
            AreExplorerHiddenItemsVisible());
        snapshot = &local;
    }
    if (!snapshot->complete)
    {
        WriteDiagnosticLogEntry(L"Folder enumeration failed; retaining current entries");
        return;
    }
    auto previous = std::exchange(widget.folderEntries, snapshot->entries);
    std::unordered_map<std::wstring, size_t> previousByPath;
    for (size_t i = 0; i < previous.size(); ++i)
        previousByPath.emplace(ToUpperInvariant(previous[i].fullPath), i);
    for (auto& entry : widget.folderEntries)
    {
        const auto found = previousByPath.find(ToUpperInvariant(entry.fullPath));
        if (found != previousByPath.end())
            snowdesktop::shell_refresh::PreserveRuntime(entry, previous[found->second]);
        if (!enqueueIconLoads ||
            (entry.iconBitmap && entry.iconState == IconState::FullQuality))
            continue;
        const auto absolute = snapshot->absoluteIds.find(ToUpperInvariant(entry.fullPath));
        if (absolute == snapshot->absoluteIds.end())
            continue;
        IconLoadTask task;
        task.serial = iconLoadSerial_;
        task.widgetId = widget.id;
        task.layoutKey = ToUpperInvariant(entry.fullPath);
        task.absolutePidl.reset(ILCloneFull(absolute->second.get()));
        task.sysIconIndex = entry.sysIconIndex;
        task.parsingName = entry.fullPath;
        task.isDesktopItem = false;
        task.folderPath = entry.fullPath;
        task.phase = entry.iconState == IconState::IconReady
            ? IconLoadPhase::Phase2 : IconLoadPhase::Phase1;
        EnqueueIconLoad(std::move(task));
    }
    for (const auto& oldEntry : previous)
        if (oldEntry.iconBitmap)
            EraseD2DIconCacheForBitmap(oldEntry.iconBitmap);
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
