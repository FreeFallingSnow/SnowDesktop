#include "app/app.h"
#include "widgets/folder_member_order.h"
#include "app/shell/shell_icon_request.h"
#include "layout/folder_mapping_rules.h"
#include "widgets/file_category_auto_collect_rules.h"

// Folder-mapping enumeration and automatic file-category collection.

snowdesktop::shell_refresh::FolderSnapshot snowdesktop::shell_refresh::ReadFolder(
    const std::wstring& path, bool showHiddenItems, MetadataCache* cache)
{
    FolderSnapshot result;
    result.path = path;
    MetadataMap* metadata = cache ? &cache->folders[FolderKey(path)] : nullptr;
    std::unordered_set<std::wstring> seenMetadata;
    if (path.empty())
    {
        result.complete = true;
        result.error = ERROR_INVALID_NAME;
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
        result.error = error;
        result.complete = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        if (metadata && result.complete) metadata->clear();
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
        entry.displayName = ReadFolderDisplayName(entry.fullPath, entry.name,
            [](const std::wstring& itemPath, SHFILEINFOW& info) {
                // No icon/image-list query and no Shell work on the UI thread.
                return SHGetFileInfoW(itemPath.c_str(), 0, &info, sizeof(info),
                    SHGFI_DISPLAYNAME) != 0;
            }, entry.isDirectory);
        entry.lastWriteTime = fd.ftLastWriteTime;
        if (!entry.isDirectory)
        {
            entry.fileSize =
                (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) |
                static_cast<std::uint64_t>(fd.nFileSizeLow);
        }
        const auto key = ToUpperInvariant(entry.fullPath);
        const auto stamp = FileStamp::From(fd);
        const auto cached = metadata ? metadata->find(key) : MetadataMap::iterator{};
        if (metadata && cached != metadata->end() && cached->second.Matches(entry.fullPath, stamp))
        {
            entry.sysIconIndex = cached->second.info.iIcon;
            entry.typeName = cached->second.info.szTypeName;
            if (cached->second.absoluteId.get())
                result.absoluteIds.emplace(key, Pidl(ILCloneFull(cached->second.absoluteId.get())));
            ++cache->hits;
            seenMetadata.insert(key);
        }
        else entry.sysIconIndex = -1;
        result.entries.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    const DWORD error = GetLastError();
    result.complete = error == ERROR_NO_MORE_FILES;
    result.error = result.complete ? ERROR_SUCCESS : error;
    FindClose(hFind);
    if (metadata && result.complete)
        std::erase_if(*metadata, [&](const auto& entry) { return !seenMetadata.contains(entry.first); });
    return result;
}

void DesktopApp::EnumerateFolderMappingEntries(DesktopWidget& widget,
    bool enqueueIconLoads, const snowdesktop::shell_refresh::FolderSnapshot* snapshot)
{
    if (snapshot && snowdesktop::shell_refresh::FolderKey(snapshot->path) !=
            snowdesktop::shell_refresh::FolderKey(widget.sourceFolderPath))
    {
        RequestShellRefresh();
        return;
    }
    const bool refreshIcons = false;
    if (!snapshot)
    {
        RequestFolderRefresh({widget.sourceFolderPath});
        return;
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
        if (refreshIcons)
            entry.iconState = IconState::Loading;
        if (!enqueueIconLoads ||
            (entry.iconBitmap && entry.iconState == IconState::FullQuality))
            continue;
        const auto absolute = snapshot->absoluteIds.find(ToUpperInvariant(entry.fullPath));
        IconLoadTask task;
        task.sourceStamp = snowdesktop::shell_icon_request::Stamp(entry);
        task.serial = iconLoadSerial_;
        task.widgetId = widget.id;
        task.layoutKey = ToUpperInvariant(entry.fullPath);
        if (absolute != snapshot->absoluteIds.end())
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
    snowdesktop::folder_member_order::Refresh(widget, ToUpperInvariant);
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
 * @brief 保证每个类别只由最后选择它的组件自动收纳。
 * @param activeWidgetIndex 当前激活的组件索引。
 */
bool DesktopApp::EnforceAutoCollectFileCategorySelection(size_t activeWidgetIndex)
{
    return !snowdesktop::file_category_auto_collect_rules::EnforceExclusiveSelection(
        widgets_, activeWidgetIndex, GetCategoryOrder(categorySettings_)).empty();
}

/**
 * @brief 应用所有开启了 autoCollect 的文件分类组件的自动收集。
 */
bool DesktopApp::ApplyAutoCollectFileCategoryWidgets()
{
    bool changed = false;
    // Remember every collected membership so items that really leave a
    // component can be re-placed instead of reviving their stale cell.
    std::unordered_set<std::wstring> ownedBefore;
    for (const auto& widget : widgets_)
        for (const auto& key : widget.itemKeys)
            if (!key.empty()) ownedBefore.insert(ToUpperInvariant(key));
    // Normalize persisted selections too, without switching off unrelated scopes.
    for (size_t i = 0; i < widgets_.size(); ++i)
        changed = EnforceAutoCollectFileCategorySelection(i) || changed;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].type != DesktopWidgetType::FileCategories)
            continue;

        FileCategories collector(&widgets_[i], this);
        changed = collector.PruneUncollectableItems() || changed;
    }
    RefreshCollectedKeysCache();
    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].type == DesktopWidgetType::FileCategories &&
            widgets_[i].autoCollect)
            changed = CollectFileCategoryWidget(i, false) || changed;
    if (!ownedBefore.empty())
    {
        RefreshCollectedKeysCache();
        std::vector<std::wstring> released;
        for (const auto& key : ownedBefore)
            if (!collectedKeysCache_.contains(key))
                released.push_back(key);
        if (!released.empty())
            changed = PlaceReleasedDesktopItems(released) || changed;
    }
    return changed;
}

// ── 组件创建辅助函数 ──────────────────────────────────

/**
 * @brief 生成一个新的唯一组件 ID。
 * @return 组件 ID 字符串。
 */
