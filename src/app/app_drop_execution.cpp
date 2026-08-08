#include "app.h"
#include "../widgets/collection_group_rules.h"
#include "../folder_self_drop_rules.h"
#include "../item_location.h"

// Internal and file-backed drop-plan execution.

bool DesktopApp::IsSelfContainedFolderDrop(
    const std::vector<std::wstring>& sourcePaths,
    const std::wstring& targetFolder) const
{
    if (sourcePaths.empty() || targetFolder.empty())
        return false;

    std::vector<std::wstring> candidates;
    candidates.reserve(sourcePaths.size() * 2);
    for (const auto& path : sourcePaths)
    {
        if (path.empty())
            continue;
        candidates.push_back(path);
        // 仅对 .lnk 源做目标解析（普通目录直接用原路径比较），
        // 避免每次拖拽悬停都创建 IShellLink COM 对象。
        const wchar_t* extension =
            PathFindExtensionW(path.c_str());
        if (extension &&
            _wcsicmp(extension, L".lnk") == 0)
        {
            const snowdesktop::item_location::FolderTarget resolved =
                snowdesktop::item_location::
                    ResolveFolderTarget(path);
            if (!resolved.path.empty())
                candidates.push_back(
                    resolved.path);
        }
    }
    return snowdesktop::folder_self_drop_rules::
        IsSelfContainedFolderDrop(candidates, targetFolder);
}

bool DesktopApp::ExecuteDropPipeline(const DragSourceList& sourceList,
    const DropPreviewList& preview,
    FileOperationCompletion completion)
{
    const bool sourceFromDock = std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
        [](const DragSourceEntry& entry) { return entry.fromDock; });
    if (sourceList.Empty()) return false;
    // A completely full desktop produces no visible landing preview. File
    // drops must still be materialized; ReloadItems will allocate virtual
    // overflow pages for the newly created desktop entries.
    if (preview.Empty() &&
        !(preview.fileBacked && preview.targetKind == DropTargetKind::Desktop))
        return false;
    if (preview.targetKind == DropTargetKind::Desktop &&
        preview.action == DropAction::Move &&
        IsAutoCollectFileCategorySource(sourceList))
        return false;
    std::unordered_set<std::wstring> movedDockEntries;
    if ((preview.action == DropAction::Move ||
         preview.consumeDockSource) && sourceFromDock)
    {
        for (const auto& entry : sourceList.entries)
            if (entry.fromDock && !entry.dockReference.empty())
                movedDockEntries.insert(
                    std::to_wstring(static_cast<int>(entry.dockEntryType)) +
                    L":" + ToUpperInvariant(entry.dockReference));
    }

    auto finish = [this,
        movedDockEntries = std::move(movedDockEntries),
        completion = std::move(completion)](bool succeeded) mutable {
        if (succeeded && !movedDockEntries.empty())
        {
            const size_t previousSize = dockEntries_.size();
            std::erase_if(dockEntries_, [&](const DockEntry& entry) {
                const std::wstring key =
                    std::to_wstring(static_cast<int>(entry.type)) +
                    L":" + ToUpperInvariant(entry.reference);
                return movedDockEntries.contains(key);
            });
            RefreshCollectedKeysCache();
            if (dockEntries_.size() != previousSize)
            {
                SaveLayoutSlots();
                RebuildContainersAndItems();
                LayoutItems();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (completion)
            completion(succeeded);
    };

    if (preview.fileBacked)
        return ExecuteFileBackedDropPlan(
            sourceList, preview, std::move(finish));

    const bool executed = ExecuteInternalDropPlan(sourceList, preview);
    finish(executed);
    return executed;
}

/**
 * @brief 执行内部拖拽放置计划（桌面间移动或组件间重排）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
bool DesktopApp::ExecuteInternalDropPlan(const DragSourceList& sourceList,
    const DropPreviewList& preview)
{
    auto sourceMemberIndices = [&]() {
        std::vector<size_t> indices;
        for (const auto& entry : sourceList.entries)
            if (entry.memberIndex != static_cast<size_t>(-1))
                indices.push_back(entry.memberIndex);
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    };

    if (preview.targetKind == DropTargetKind::Desktop)
    {
        if (sourceList.hasCollectionGroupEntries ||
            sourceList.hasFileGroupEntries)
        {
            bool changed = false;
            for (const auto& landing : preview.landings)
            {
                auto it = std::find_if(sourceList.entries.begin(),
                    sourceList.entries.end(),
                    [&](const DragSourceEntry& entry) {
                        return entry.sourceIndex == landing.sourceIndex;
                    });
                if (it == sourceList.entries.end() ||
                    it->widgetId.empty())
                    continue;
                changed =
                    (sourceList.hasCollectionGroupEntries
                        ? ReleaseCollectionFromGroup(
                            it->widgetId, landing.cell)
                        : ReleaseWidgetFromFileGroup(
                            it->widgetId, landing.cell)) ||
                    changed;
            }
            return changed;
        }
        RemoveDesktopKeysFromWidgets(sourceList.DesktopKeys());
        bool changed = false;
        for (const auto& landing : preview.landings)
        {
            auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
            if (it == sourceList.entries.end() || it->desktopIndex >= items_.size()) continue;
            items_[it->desktopIndex].gridCell = landing.cell;
            items_[it->desktopIndex].slot = SlotFromCell(gridPages_, landing.cell);
            changed = true;
        }
        if (changed)
        {
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return changed;
    }

    if (preview.targetKind == DropTargetKind::KeyedWidget && preview.targetWidget)
    {
        WidgetContainer* targetWidget =
            dynamic_cast<WidgetContainer*>(
                preview.targetContainer);
        if (targetWidget &&
            targetWidget->GetWidgetData() !=
                preview.targetWidget)
            targetWidget = nullptr;
        for (auto& container : containers_)
        {
            if (targetWidget) break;
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (widget && widget->GetWidgetData() == preview.targetWidget)
            {
                targetWidget = widget;
                break;
            }
        }
        CollectionGroup* collectionGroupProxy = nullptr;
        if (!targetWidget)
        {
            collectionGroupProxy =
                dynamic_cast<CollectionGroup*>(
                    preview.targetContainer);
            DesktopWidget* groupData =
                collectionGroupProxy
                    ? collectionGroupProxy->GetWidgetData()
                    : nullptr;
            if (groupData &&
                std::find(
                    groupData->childWidgetIds.begin(),
                    groupData->childWidgetIds.end(),
                    preview.targetWidget->id) !=
                    groupData->childWidgetIds.end())
                targetWidget = collectionGroupProxy;
        }
        if (!targetWidget) return false;

        if ((sourceList.hasCollectionGroupEntries &&
             preview.targetWidget->type ==
                DesktopWidgetType::CollectionGroup) ||
            (sourceList.hasFileGroupEntries &&
             preview.targetWidget->type ==
                DesktopWidgetType::FileGroup))
        {
            std::vector<std::wstring> movingIds;
            for (const auto& entry : sourceList.entries)
            {
                const bool matchingEntry =
                    sourceList.hasCollectionGroupEntries
                        ? entry.kind ==
                            DropSourceKind::CollectionGroupEntry
                        : entry.kind ==
                            DropSourceKind::FileGroupEntry;
                if (matchingEntry &&
                    !entry.widgetId.empty())
                    movingIds.push_back(entry.widgetId);
            }
            if (movingIds.empty()) return false;

            DesktopWidget& targetGroup =
                *preview.targetWidget;
            const std::wstring previousTargetActive =
                targetGroup.activeCategoryId;
            size_t removedBefore = 0;
            for (const auto& id : movingIds)
            {
                auto it = std::find(
                    targetGroup.childWidgetIds.begin(),
                    targetGroup.childWidgetIds.end(), id);
                if (it != targetGroup.childWidgetIds.end() &&
                    static_cast<size_t>(std::distance(
                        targetGroup.childWidgetIds.begin(), it)) <
                        preview.insertIndex)
                    ++removedBefore;
            }

            for (auto& widget : widgets_)
            {
                if (widget.type !=
                    preview.targetWidget->type)
                    continue;
                for (const auto& id : movingIds)
                {
                    std::erase(widget.childWidgetIds, id);
                    if (widget.activeCategoryId == id)
                        widget.activeCategoryId =
                            widget.childWidgetIds.empty()
                                ? L""
                                : widget.childWidgetIds.front();
                }
            }
            size_t insertAt =
                preview.insertIndex > removedBefore
                    ? preview.insertIndex - removedBefore
                    : 0;
            insertAt = std::min(
                insertAt, targetGroup.childWidgetIds.size());
            targetGroup.childWidgetIds.insert(
                targetGroup.childWidgetIds.begin() +
                    static_cast<std::ptrdiff_t>(insertAt),
                movingIds.begin(), movingIds.end());
            targetGroup.activeCategoryId =
                snowdesktop::collection_group_rules::
                    ResolveActiveItem(
                        targetGroup.childWidgetIds,
                        previousTargetActive);
            for (auto& container : containers_)
            {
                if (auto* group =
                        dynamic_cast<CollectionGroup*>(
                            container.get()))
                    group->InvalidateFilterCache();
                else if (auto* fileGroup =
                             dynamic_cast<FileGroup*>(
                                 container.get()))
                    fileGroup->InvalidateHostedView();
            }
            EnsureNavTabOrder();
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return true;
        }

        const bool reorderInsideCollectionGroup =
            collectionGroupProxy &&
            targetWidget->GetWidgetData() !=
                preview.targetWidget &&
            collectionGroupProxy->GetActiveCollectionId() ==
                preview.targetWidget->id;
        if (sourceList.origin == targetWidget &&
            preview.action == DropAction::Move &&
            (!collectionGroupProxy ||
                reorderInsideCollectionGroup))
        {
            if (collectionGroupProxy &&
                targetWidget->GetWidgetData() !=
                    preview.targetWidget)
            {
                std::unordered_set<std::wstring> movingKeys;
                for (const auto& key :
                    sourceList.DesktopKeys())
                    movingKeys.insert(
                        ToUpperInvariant(key));
                if (movingKeys.empty()) return false;

                auto& targetKeys =
                    preview.targetWidget->itemKeys;
                std::vector<std::wstring> moving;
                size_t removedBefore = 0;
                const bool moveBetweenCollections =
                    std::any_of(
                        sourceList.entries.begin(),
                        sourceList.entries.end(),
                        [&](const DragSourceEntry& entry) {
                            return entry.kind ==
                                    DropSourceKind::DesktopIcon &&
                                !entry.widgetId.empty() &&
                                entry.widgetId !=
                                    preview.targetWidget->id;
                        });

                if (moveBetweenCollections)
                {
                    for (const auto& entry :
                        sourceList.entries)
                    {
                        if (entry.kind !=
                                DropSourceKind::DesktopIcon ||
                            entry.widgetId.empty() ||
                            entry.desktopKey.empty())
                            continue;
                        const size_t sourceIndex =
                            FindWidgetIndexById(entry.widgetId);
                        if (sourceIndex >= widgets_.size() ||
                            widgets_[sourceIndex].type !=
                                DesktopWidgetType::Collection)
                            continue;
                        auto& sourceKeys =
                            widgets_[sourceIndex].itemKeys;
                        auto sourceIt = std::find_if(
                            sourceKeys.begin(), sourceKeys.end(),
                            [&](const std::wstring& key) {
                                return ToUpperInvariant(key) ==
                                    ToUpperInvariant(
                                        entry.desktopKey);
                            });
                        if (sourceIt == sourceKeys.end())
                            continue;
                        moving.push_back(*sourceIt);
                        sourceKeys.erase(sourceIt);
                    }
                }
                else
                {
                    for (size_t i = 0;
                        i < targetKeys.size(); ++i)
                    {
                        if (!movingKeys.contains(
                                ToUpperInvariant(
                                    targetKeys[i])))
                            continue;
                        if (i < preview.insertIndex)
                            ++removedBefore;
                        moving.push_back(targetKeys[i]);
                    }
                }
                if (moving.empty()) return false;

                std::erase_if(
                    targetKeys,
                    [&](const std::wstring& key) {
                        return movingKeys.contains(
                            ToUpperInvariant(key));
                    });
                size_t insertAt =
                    preview.insertIndex > removedBefore
                        ? preview.insertIndex - removedBefore
                        : 0;
                insertAt = std::min(
                    insertAt, targetKeys.size());
                targetKeys.insert(
                    targetKeys.begin() +
                        static_cast<std::ptrdiff_t>(insertAt),
                    moving.begin(), moving.end());
                collectionGroupProxy->InvalidateFilterCache();
                RefreshCollectedKeysCache();
                RefreshOpenCollectionPopupGeometry();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, TRUE);
                return true;
            }

            std::vector<size_t> indices = sourceMemberIndices();
            if (indices.empty())
                indices = targetWidget->GetSelectedMemberIndices();
            targetWidget->ReorderMembers(indices, preview.insertIndex);
            if (targetWidget ==
                dockFolderPopupContainer_.get())
                CommitDockFolderPopupStateToSource();
            targetWidget->InvalidateSlots();
            return true;
        }

        WidgetContainer* originWidget = dynamic_cast<WidgetContainer*>(sourceList.origin);
        DesktopWidget* originData = originWidget ? originWidget->GetWidgetData() : nullptr;
        size_t inserted = 0;
        for (const auto& landing : preview.landings)
        {
            auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
            if (it == sourceList.entries.end() || it->desktopKey.empty()) continue;
            std::wstring key = ToUpperInvariant(it->desktopKey);
            if (!targetWidget->AllowsDesktopKey(key)) continue;

            if (preview.action == DropAction::Move)
            {
                if (originData)
                    RemoveDesktopKeysFromWidgets({key});
                else
                    RemoveDesktopKeysFromWidgets({key});
            }

            auto exists = std::find_if(preview.targetWidget->itemKeys.begin(),
                preview.targetWidget->itemKeys.end(),
                [&](const std::wstring& existing) { return ToUpperInvariant(existing) == key; });
            if (exists == preview.targetWidget->itemKeys.end())
            {
                size_t insertAt = std::min(preview.insertIndex + inserted, preview.targetWidget->itemKeys.size());
                preview.targetWidget->itemKeys.insert(
                    preview.targetWidget->itemKeys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
                ++inserted;
            }
            size_t itemIndex = FindItemIndexByKey(key);
            if (itemIndex != static_cast<size_t>(-1))
                items_[itemIndex].gridCell = preview.targetWidget->gridCell;
        }
        if (originWidget) originWidget->InvalidateSlots();
        targetWidget->InvalidateSlots();
        if (GetDesktopGrid()) GetDesktopGrid()->InvalidateSlots();
        RefreshCollectedKeysCache();
        RefreshOpenCollectionPopupGeometry();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return true;
    }

    if (preview.targetKind == DropTargetKind::FolderMapping &&
        sourceList.origin == preview.targetContainer && preview.action == DropAction::Move)
    {
        auto* targetWidget = dynamic_cast<WidgetContainer*>(preview.targetContainer);
        if (!targetWidget) return false;
        std::vector<size_t> indices = sourceMemberIndices();
        if (indices.empty())
            indices = targetWidget->GetSelectedMemberIndices();
        targetWidget->ReorderMembers(indices, preview.insertIndex);
        if (targetWidget ==
            dockFolderPopupContainer_.get())
            CommitDockFolderPopupStateToSource();
        return true;
    }

    return false;
}

/**
 * @brief 将文件实际复制/移动/创建快捷方式到桌面目录。
 * @param sourceList 拖拽源列表。
 * @param action 拖拽动作（复制/移动/链接）。
 * @param duplicateDesktopCopyNames 是否对已在桌面的文件生成副本名称。
 * @param createdPathsBySource 输出参数，记录每个源索引对应的创建路径。
 * @return 操作成功返回 true。
 */
bool DesktopApp::MaterializeFilesToDesktop(const DragSourceList& sourceList,
    DropAction action, bool duplicateDesktopCopyNames,
    std::unordered_map<size_t, std::wstring>* createdPathsBySource,
    FileOperationCompletion completion)
{
    std::vector<std::wstring> paths = sourceList.FilePaths();
    if (paths.empty()) return false;
    if (createdPathsBySource)
        createdPathsBySource->clear();

    wchar_t desktopPathRaw[MAX_PATH]{};
    if (!SHGetSpecialFolderPathW(nullptr, desktopPathRaw, CSIDL_DESKTOPDIRECTORY, FALSE))
        return false;
    std::wstring desktopPath = TrimTrailingPathSeparators(desktopPathRaw);

    auto sameParentAsDesktop = [&](const std::wstring& path) -> bool {
        wchar_t parent[MAX_PATH]{};
        wcscpy_s(parent, path.c_str());
        if (!PathRemoveFileSpecW(parent)) return false;
        return PathsEqualInsensitive(parent, desktopPath);
    };

    std::unordered_set<std::wstring> reservedDestinations;
    auto makeUniqueCopyPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        DWORD attrs = GetFileAttributesW(path.c_str());
        bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);

        std::wstring stem = fileName ? fileName : L"";
        std::wstring ext;
        if (!isDir)
        {
            wchar_t stemBuf[MAX_PATH]{};
            wcscpy_s(stemBuf, stem.c_str());
            PathRemoveExtensionW(stemBuf);
            stem = stemBuf;
            const wchar_t* extPtr = PathFindExtensionW(fileName);
            ext = extPtr ? extPtr : L"";
        }

        for (int i = 1; i < 1000; ++i)
        {
            std::wstring name = i <= 1
                    ? stem + _LW("app.grid.copy_suffix") + ext
                : stem + _LFW("app.grid.copy_suffix_num", std::to_wstring(i)) + ext;
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            const std::wstring candidate = dst;
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES &&
                !reservedDestinations.contains(
                    ToUpperInvariant(candidate)))
            {
                reservedDestinations.insert(
                    ToUpperInvariant(candidate));
                return std::wstring(dst);
            }
        }
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath.c_str(), (stem + _LW("app.grid.copy_suffix_1000") + ext).c_str());
        return std::wstring(fallback);
    };

    auto makeUniqueShortcutPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        wchar_t stemBuf[MAX_PATH]{};
        wcscpy_s(stemBuf, fileName ? fileName : L"");
        PathRemoveExtensionW(stemBuf);
        std::wstring stem = stemBuf[0] != L'\0' ? stemBuf : _LW("widget.shortcut");

        for (int i = 1; i < 1000; ++i)
        {
            std::wstring name = i <= 1
                ? stem + L".lnk"
                : stem + L" (" + std::to_wstring(i) + L").lnk";
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES)
                return std::wstring(dst);
        }
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath.c_str(), (stem + L" (1000).lnk").c_str());
        return std::wstring(fallback);
    };

    bool operated = false;
    if (action == DropAction::Link)
    {
        for (const auto& source : sourceList.entries)
        {
            const auto& path = source.filePath;
            if (path.empty()) continue;

            std::wstring dst = makeUniqueShortcutPath(path);
            ComPtr<IShellLinkW> shellLink;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
            {
                shellLink->SetPath(path.c_str());
                shellLink->SetWorkingDirectory(desktopPath.c_str());
                ComPtr<IPersistFile> persistFile;
                if (SUCCEEDED(shellLink.As(&persistFile)) &&
                    SUCCEEDED(persistFile->Save(dst.c_str(), TRUE)))
                {
                    if (createdPathsBySource)
                        (*createdPathsBySource)[source.sourceIndex] = dst;
                    operated = true;
                }
            }
        }
    }
    else if (action == DropAction::Copy)
    {
        std::vector<snowdesktop::ShellFileOperationStep> steps;
        std::vector<std::wstring> normalCopies;
        for (const auto& path : paths)
        {
            if (duplicateDesktopCopyNames && sameParentAsDesktop(path))
            {
                steps.push_back({
                    FO_COPY,
                    { path },
                    makeUniqueCopyPath(path),
                    static_cast<FILEOP_FLAGS>(
                        FOF_NOCONFIRMATION |
                        FOF_NOCONFIRMMKDIR |
                        FOF_NOERRORUI |
                        FOF_RENAMEONCOLLISION) });
            }
            else
                normalCopies.push_back(path);
        }
        if (!normalCopies.empty())
        {
            steps.push_back({
                FO_COPY,
                std::move(normalCopies),
                desktopPath,
                static_cast<FILEOP_FLAGS>(
                    FOF_NOCONFIRMATION |
                    FOF_NOCONFIRMMKDIR |
                    FOF_NOERRORUI |
                    FOF_RENAMEONCOLLISION) });
        }
        operated = QueueShellFileOperation(
            std::move(steps), std::move(completion));
    }
    else
    {
        std::vector<snowdesktop::ShellFileOperationStep> steps;
        steps.push_back({
            FO_MOVE,
            std::move(paths),
            desktopPath,
            static_cast<FILEOP_FLAGS>(
                FOF_NOCONFIRMATION |
                FOF_NOCONFIRMMKDIR |
                FOF_NOERRORUI |
                FOF_RENAMEONCOLLISION) });
        operated = QueueShellFileOperation(
            std::move(steps), std::move(completion));
    }
    return operated;
}

/**
 * @brief 将文件实际复制/移动/创建快捷方式到指定文件夹。
 * @param sourceList 拖拽源列表。
 * @param folderPath 目标文件夹路径。
 * @param action 拖拽动作。
 * @return 操作成功返回 true。
 */
bool DesktopApp::MaterializeFilesToFolder(const DragSourceList& sourceList,
    const std::wstring& folderPath, DropAction action,
    FileOperationCompletion completion)
{
    std::vector<std::wstring> paths = sourceList.FilePaths();
    if (paths.empty() || folderPath.empty()) return false;
    // 文件夹不能拖进它自身或其子目录：SHFileOperation 会成功地把
    // 文件夹递归复制进自己（如 foo 拖入自身弹窗产生 foo\foo），
    // 而 Explorer 会直接拒绝。这里统一拦截所有落地入口。
    if (IsSelfContainedFolderDrop(paths, folderPath))
        return false;

    std::wstring folder = folderPath;
    if (!folder.empty() && folder.back() != L'\\') folder += L'\\';

    if (action == DropAction::Link)
    {
        bool createdAny = false;
        for (const auto& path : paths)
        {
            std::wstring name = PathFindFileNameW(path.c_str());
            std::wstring stem = name;
            if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".lnk") == 0)
                stem = stem.substr(0, stem.size() - 4);

            std::wstring linkPath;
            for (int i = 1; i < 1000; ++i)
            {
                linkPath = folder + stem + (i == 1 ? L".lnk" : L" (" + std::to_wstring(i) + L").lnk");
                if (GetFileAttributesW(linkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                    break;
            }

            ComPtr<IShellLinkW> shellLink;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                continue;

            shellLink->SetPath(path.c_str());
            shellLink->SetWorkingDirectory(folder.c_str());
            ComPtr<IPersistFile> persistFile;
            if (SUCCEEDED(shellLink.As(&persistFile)) &&
                SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE)))
                createdAny = true;
        }
        return createdAny;
    }

    std::vector<snowdesktop::ShellFileOperationStep> steps;
    steps.push_back({
        static_cast<UINT>(
            action == DropAction::Move ? FO_MOVE : FO_COPY),
        std::move(paths),
        folder,
        static_cast<FILEOP_FLAGS>(
            FOF_NOCONFIRMATION |
            FOF_NOERRORUI |
            FOF_RENAMEONCOLLISION) });
    return QueueShellFileOperation(
        std::move(steps), std::move(completion));
}

/**
 * @brief 缓存待处理的放置结果，供后续 ApplyPendingPlacement 使用。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @param existingKeys 放置前的桌面键快照。
 * @param createdPathsBySource 可选参数，记录每个源索引对应的创建路径。
 */
void DesktopApp::StorePendingLandingCache(const DragSourceList& sourceList,
    const DropPreviewList& preview, const std::unordered_set<std::wstring>& existingKeys,
    const std::unordered_map<size_t, std::wstring>* createdPathsBySource)
{
    pendingLandingCache_.Clear();
    pendingLandingCache_.existingDesktopKeys = existingKeys;
    pendingLandingCache_.tick = GetTickCount();

    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell &&
            landing.kind != DropLandingKind::WidgetIndex)
            continue;
        auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
            [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
        if (it == sourceList.entries.end()) continue;

        PendingLandingEntry entry;
        entry.sourceIndex = it->sourceIndex;
        entry.action = preview.action;
        entry.kind = landing.kind;
        entry.sourcePath = it->filePath;
        entry.sourceName = !it->filePath.empty() ? FileNameFromPath(it->filePath) : it->displayName;
        if (createdPathsBySource)
        {
            auto created = createdPathsBySource->find(it->sourceIndex);
            if (created != createdPathsBySource->end())
                entry.createdPath = created->second;
        }
        entry.cell = landing.kind == DropLandingKind::DesktopCell ? landing.cell : landing.cell;
        entry.insertIndex = landing.insertIndex;
        entry.widget = landing.widget;
        entry.widgetId = landing.widgetId;
        pendingLandingCache_.entries.push_back(entry);
    }
    pendingLandingCache_.active = !pendingLandingCache_.entries.empty();
}

/**
 * @brief 执行基于文件系统的拖拽放置计划（复制/移动到桌面或文件夹映射）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
bool DesktopApp::ExecuteFileBackedDropPlan(const DragSourceList& sourceList,
    const DropPreviewList& preview,
    FileOperationCompletion completion)
{
    const std::vector<std::wstring> desktopKeys =
        sourceList.DesktopKeys();

    if (preview.targetKind == DropTargetKind::FolderMapping && preview.targetWidget)
    {
        const std::wstring targetWidgetId =
            preview.targetWidget->id;
        std::unordered_set<std::wstring> targetExistingPaths;
        for (const auto& entry : preview.targetWidget->folderEntries)
            targetExistingPaths.insert(
                ToUpperInvariant(entry.fullPath));
        const size_t targetInsertIndex = preview.insertIndex;
        const DropAction action = preview.action;

        auto operationCompletion = [this,
            action,
            desktopKeys,
            targetWidgetId,
            targetExistingPaths = std::move(targetExistingPaths),
            targetInsertIndex,
            completion = std::move(completion)](bool succeeded) mutable {
            if (succeeded)
            {
                if (action == DropAction::Move)
                    RemoveDesktopKeysFromWidgets(desktopKeys);

                ReloadItems(false);
                const size_t targetWidgetIndex =
                    FindWidgetIndexById(targetWidgetId);
                if (targetWidgetIndex < widgets_.size())
                {
                    auto& target = widgets_[targetWidgetIndex];
                    std::vector<FolderEntry> inserted;
                    for (auto it = target.folderEntries.begin();
                        it != target.folderEntries.end();)
                    {
                        if (targetExistingPaths.contains(
                                ToUpperInvariant(it->fullPath)))
                        {
                            ++it;
                            continue;
                        }
                        inserted.push_back(std::move(*it));
                        it = target.folderEntries.erase(it);
                    }
                    if (!inserted.empty())
                    {
                        const size_t insertAt = std::min(
                            targetInsertIndex,
                            target.folderEntries.size());
                        target.folderEntries.insert(
                            target.folderEntries.begin() +
                                static_cast<std::ptrdiff_t>(insertAt),
                            std::make_move_iterator(inserted.begin()),
                            std::make_move_iterator(inserted.end()));
                        target.itemKeys.clear();
                        target.itemKeys.reserve(
                            target.folderEntries.size());
                        for (const auto& entry : target.folderEntries)
                            target.itemKeys.push_back(entry.fullPath);
                        SaveLayoutSlots();
                        RebuildContainersAndItems();
                        LayoutItems();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
                if (dockFolderPopupOpen_)
                    RefreshDockFolderPopup();
            }
            if (completion)
                completion(succeeded);
        };

        if (action == DropAction::Link)
        {
            const bool succeeded = MaterializeFilesToFolder(
                sourceList,
                preview.targetWidget->sourceFolderPath,
                action,
                {});
            operationCompletion(succeeded);
            return succeeded;
        }

        const bool queued = MaterializeFilesToFolder(
            sourceList,
            preview.targetWidget->sourceFolderPath,
            action,
            operationCompletion);
        if (!queued)
            operationCompletion(false);
        return queued;
    }

    const bool duplicateCopyNames =
        preview.action == DropAction::Copy && sourceList.hasDesktopIcons &&
        !sourceList.hasExternalFiles;
    const std::unordered_set<std::wstring> existingKeys =
        SnapshotDesktopKeys();
    std::unordered_map<size_t, std::wstring> createdPathsBySource;
    const DropAction action = preview.action;

    if (action == DropAction::Link)
    {
        const bool succeeded = MaterializeFilesToDesktop(
            sourceList, action, duplicateCopyNames,
            &createdPathsBySource, {});
        if (succeeded)
        {
            StorePendingLandingCache(
                sourceList, preview, existingKeys,
                &createdPathsBySource);
            pendingLandingCache_.tick = GetTickCount();
            ReloadItems(false);
            if (dockFolderPopupOpen_)
                RefreshDockFolderPopup();
        }
        else
        {
            pendingLandingCache_.Clear();
        }
        if (completion)
            completion(succeeded);
        return succeeded;
    }

    StorePendingLandingCache(
        sourceList, preview, existingKeys, nullptr);
    PendingLandingCache landingCache =
        std::move(pendingLandingCache_);
    pendingLandingCache_.Clear();

    auto operationCompletion = [this,
        action,
        hasDesktopIcons = sourceList.hasDesktopIcons,
        desktopKeys,
        landingCache = std::move(landingCache),
        completion = std::move(completion)](bool succeeded) mutable {
        if (succeeded)
        {
            landingCache.tick = GetTickCount();
            pendingLandingCache_ = std::move(landingCache);
            if (action == DropAction::Move && hasDesktopIcons)
                RemoveDesktopKeysFromWidgets(desktopKeys);
            ReloadItems(false);
            if (dockFolderPopupOpen_)
                RefreshDockFolderPopup();
        }
        if (completion)
            completion(succeeded);
    };

    const bool queued = MaterializeFilesToDesktop(
        sourceList, action, duplicateCopyNames,
        nullptr, operationCompletion);
    if (!queued)
    {
        pendingLandingCache_.Clear();
        operationCompletion(false);
    }
    return queued;
}

/**
 * @brief 在 D2D 设备上下文上绘制拖拽放置预览（高亮目标区域）。
 * @param ctx D2D 设备上下文。
 * @param preview 拖拽预览列表。
 */
