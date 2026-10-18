#include "app/app.h"
#include "drag_drop/pending_drop_completion.h"
#include "widgets/collection_group_rules.h"
#include "widgets/widget_group_drop.h"
#include "layout/folder_self_drop_rules.h"
#include "layout/item_location.h"
#include "layout/category_collection_rules.h"
#include "desktop/desktop_category_item_rules.h"
#include "drag_drop/empty_group_drop_rules.h"
#include "shell/shortcut_category_target.h"
#include "drag_drop/drop_staging.h"
#include "drag_drop/file_backed_drop_request.h"
#include "drag_drop/desktop_file_materialization.h"
#include "drag_drop/file_drop_execution.h"
#include "drag_drop/drop_pipeline.h"
#include "drag_drop/internal_desktop_drop.h"

// Internal and file-backed drop-plan execution.

bool DesktopApp::IsSelfContainedFolderDrop(
    const std::vector<std::wstring>& sourcePaths,
    const std::wstring& targetFolder) const
{
    return snowdesktop::desktop_file_materialization::IsSelfContainedFolderDrop(sourcePaths, targetFolder);
}

bool DesktopApp::ExecuteDropPipeline(const DragSourceList& sourceList,
    const DropPreviewList& requestedPreview,
    FileOperationCompletion completion,
    bool executeSynchronously,
    std::shared_ptr<snowdesktop::ShellFileOperationResult> result)
{
    snowdesktop::drop_pipeline::Services services;
    services.runtimeLive = [this](Container* candidate) {
        if (!candidate || candidate == dockFolderPopupContainer_.get()) return true;
        for (const auto& container : containers_)
        {
            if (container.get() == candidate) return true;
            if (auto* group = dynamic_cast<FileGroup*>(container.get());
                group && group->GetActiveSourceContainer() == candidate) return true;
        }
        return false;
    };
    services.containsPrograms = [this](const DragSourceList& sources) {
        return snowdesktop::drop_pipeline::ContainsPrograms(sources,
            GetProgramCategoryExtensions(categorySettings_), [this](const auto& key) -> DesktopItem* {
                const auto index = FindItemIndexByKey(key);
                return index < items_.size() ? &items_[index] : nullptr;
            }, ToUpperInvariant);
    };
    services.confirmConversion = [this](bool programs) {
        const TASKDIALOG_BUTTON buttons[] = {
            {IDYES, _LW("widget.group.convert_continue")}, {IDCANCEL, _LW("app.settings.cancel")}};
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = controlHwnd_ ? controlHwnd_ : hwnd_;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.pszWindowTitle = L"SnowDesktop";
        dialog.pszMainInstruction = _LW("widget.group.convert_title");
        dialog.pszContent = programs ? _LW("widget.group.convert_programs_message") :
            _LW("widget.group.convert_files_message");
        dialog.pButtons = buttons;
        dialog.cButtons = static_cast<UINT>(std::size(buttons));
        dialog.nDefaultButton = IDCANCEL;
        int selected = IDCANCEL;
        return SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)) && selected == IDYES;
    };
    services.confirmPrograms = [this] {
        const TASKDIALOG_BUTTON buttons[] = {
            {IDYES, _LW("app.settings.widgets_enable")}, {IDCANCEL, _LW("app.settings.cancel")}};
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = controlHwnd_ ? controlHwnd_ : hwnd_;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.pszWindowTitle = L"SnowDesktop";
        dialog.pszMainInstruction = _LW("app.settings.collect_programs_confirm_title");
        dialog.pszContent = _LW("app.settings.collect_programs_confirm_message");
        dialog.pButtons = buttons;
        dialog.cButtons = static_cast<UINT>(std::size(buttons));
        dialog.nDefaultButton = IDCANCEL;
        int selected = IDCANCEL;
        return SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)) && selected == IDYES;
    };
    services.exiting = [this] { return exitRequested_; };
    services.createConvertedView = [this](DesktopWidget* widget) {
        std::shared_ptr<Widget> view(CreateWidget(widget, this));
        return snowdesktop::drop_pipeline::ConvertedView{view, dynamic_cast<WidgetContainer*>(view.get())};
    };
    services.saveLayout = [this] { SaveLayoutSlots(); };
    services.convertedLayout = [this] { LayoutItems(); SaveLayoutSlots(); InvalidateRect(hwnd_, nullptr, FALSE); };
    services.programsEnabled = [this] { return categorySettings_.collectProgramsEnabled; };
    services.enablePrograms = [this] {
        CategorySettings enabled = categorySettings_;
        enabled.collectProgramsEnabled = true;
        if (!SaveCategorySettings(GetCategorySettingsPath().c_str(), enabled)) return false;
        categorySettings_ = enabled;
        // Preserve an unrelated category editor draft while enabling collection.
        if (settingsController_)
        {
            const auto reloaded = settingsController_->Reload(snowdesktop::SettingsReloadPolicy::PreservePendingChanges);
            if (!reloaded.Succeeded() && reloaded.status != snowdesktop::SettingsActionStatus::Busy)
            {
                const std::wstring message = L"Program collection setting saved, but settings reload failed: " + reloaded.message;
                WriteDiagnosticLogEntry(message.c_str());
            }
            if (!reloaded.Succeeded())
                if (const auto snapshot = settingsController_->Snapshot(); snapshot && !snapshot->values.category.collectProgramsEnabled)
                {
                    auto draft = snapshot->values.category;
                    draft.collectProgramsEnabled = true;
                    settingsController_->UpdateCategory(std::move(draft), snowdesktop::SettingsUpdateMode::Draft);
                }
        }
        for (auto& container : containers_)
        {
            if (auto* categories = dynamic_cast<FileCategories*>(container.get())) categories->InvalidateCategoryCache();
            else if (auto* group = dynamic_cast<FileGroup*>(container.get())) group->InvalidateHostedView();
        }
        return true;
    };
    services.autoCollectSource = [this](const auto& sources) { return IsAutoCollectFileCategorySource(sources); };
    services.normalize = ToUpperInvariant;
    services.refreshCollectedKeys = [this] { RefreshCollectedKeysCache(); };
    services.dockChanged = [this] {
        SaveLayoutSlots(); RebuildContainersAndItems(); LayoutItems(); InvalidateRect(hwnd_, nullptr, FALSE);
    };
    services.files = [this](const auto& sources, const auto& preview, auto done, bool sync, auto outputs) {
        return ExecuteFileBackedDropPlan(sources, preview, std::move(done), sync, std::move(outputs));
    };
    services.internal = [this](const auto& sources, const auto& preview) { return ExecuteInternalDropPlan(sources, preview); };
    services.collectionTitle = _LW("widget.collection");
    services.filesTitle = _LW("widget.desktop_files");
    return snowdesktop::drop_pipeline::Execute(sourceList, requestedPreview, widgets_, dockEntries_, services,
        std::move(completion), executeSynchronously, std::move(result));
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
            return snowdesktop::widget_group_drop::ReleasePlan(sourceList, preview,
                [this](const auto& id, GridCell cell, bool collection) {
                    return collection ? ReleaseCollectionFromGroup(id, cell) : ReleaseWidgetFromFileGroup(id, cell);
                });
        }
        snowdesktop::internal_desktop_drop::Services services;
        services.removeKeys = [this](const auto& keys) { RemoveDesktopKeysFromWidgets(keys); };
        services.findWidget = [this](const auto& id) { return FindWidgetIndexById(id); };
        services.findItem = [this](const auto& key) { return FindItemIndexByKey(key); };
        services.slot = [this](const auto& cell) { return SlotFromCell(gridPages_, cell); };
        services.prepareLayout = [this] {
            // Releasing existing items changes widget membership without a
            // Shell file event, so auto-collection must run at this boundary.
            ApplyAutoCollectFileCategoryWidgets();
            RefreshCollectedKeysCache();
        };
        services.mapPages = [this] { ApplyPageMapping(); };
        services.committed = [this] {
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        };
        return snowdesktop::internal_desktop_drop::Execute(sourceList, preview, items_, widgets_,
            {savedPageIds_, savedPageColumns_, savedPageRows_}, services);
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
            if (!snowdesktop::widget_group_drop::ApplyPlan(widgets_, dockEntries_, sourceList, preview)) return false;
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
            NormalizeDockRecycleBinPosition();
            RefreshCollectedKeysCache();
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
        // A group exposes its active collection through a proxy, while the
        // target can resolve to that child's own runtime container. Ownership
        // still identifies a reorder; treating it as an incoming transfer
        // loses the removal-before-insertion adjustment for middle drops.
        const bool reorderActiveCollection =
            sourceList.hasOriginWidget &&
            sourceList.originWidgetType == DesktopWidgetType::CollectionGroup &&
            preview.targetWidget->type == DesktopWidgetType::Collection &&
            !sourceList.entries.empty() &&
            std::all_of(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) {
                    return entry.kind == DropSourceKind::DesktopIcon &&
                        entry.widgetId == preview.targetWidget->id && !entry.desktopKey.empty();
                });
        if ((sourceList.origin == targetWidget || reorderActiveCollection) &&
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

            std::vector<size_t> indices;
            if (reorderActiveCollection)
            {
                // Bind captured identities to current membership; a refresh
                // may have changed indices since the drag began.
                const auto& keys = preview.targetWidget->itemKeys;
                std::unordered_set<std::wstring> movingKeys;
                movingKeys.reserve(sourceList.entries.size());
                for (const auto& entry : sourceList.entries)
                    movingKeys.insert(ToUpperInvariant(entry.desktopKey));
                for (size_t i = 0; i < keys.size(); ++i)
                    if (movingKeys.contains(ToUpperInvariant(keys[i]))) indices.push_back(i);
                if (indices.empty()) return false;
            }
            else indices = sourceMemberIndices();
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
        preview.targetWidget->contentSortColumn =
            snowdesktop::list_detail_rules::Column::None;
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
    FileOperationCompletion completion,
    bool executeSynchronously,
    std::shared_ptr<snowdesktop::ShellFileOperationResult> result)
{
    wchar_t desktopPath[MAX_PATH]{};
    if (!snowdesktop::desktop_source::CopyDirectory(desktopPath)) return false;
    snowdesktop::desktop_file_materialization::Services services;
    services.isStaging = snowdesktop::drop_staging::Contains;
    services.pathsEqual = PathsEqualInsensitive;
    services.normalize = ToUpperInvariant;
    services.copyName = [](const std::wstring& stem, const std::wstring& ext, int index) {
        return stem + (index <= 1 ? std::wstring(_LW("app.grid.copy_suffix")) :
            index == 1000 ? std::wstring(_LW("app.grid.copy_suffix_1000")) :
                _LFW("app.grid.copy_suffix_num", std::to_wstring(index))) + ext;
    };
    services.shortcutName = _LW("widget.shortcut");
    services.enqueue = [this](auto request, auto done) {
        return QueueShellFileOperation(std::move(request), std::move(done));
    };
    return snowdesktop::desktop_file_materialization::Materialize(sourceList, action,
        duplicateDesktopCopyNames, TrimTrailingPathSeparators(desktopPath),
        pendingDesktopMaterializedPaths_, services, createdPathsBySource,
        std::move(completion), executeSynchronously, std::move(result));
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
    FileOperationCompletion completion,
    bool executeSynchronously,
    std::shared_ptr<snowdesktop::ShellFileOperationResult> result)
{
    snowdesktop::desktop_file_materialization::Services services;
    services.enqueue = [this](auto request, auto done) {
        return QueueShellFileOperation(std::move(request), std::move(done));
    };
    return snowdesktop::desktop_file_materialization::MaterializeFolder(sourceList, folderPath,
        action, services, std::move(completion), executeSynchronously, std::move(result));
}

/**
 * @brief 缓存待处理的放置结果，供后续 ApplyPendingPlacement 使用。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @param existingKeys 放置前的桌面键快照。
 * @param createdPathsBySource 可选参数，记录每个源索引对应的创建路径。
 */
PendingLandingCache DesktopApp::BuildPendingLandingCache(const DragSourceList& sourceList,
    const DropPreviewList& preview, const std::unordered_set<std::wstring>& existingKeys,
    const std::unordered_map<size_t, std::wstring>* createdPathsBySource)
{
    return snowdesktop::pending_drop::BuildLanding(sourceList, preview, existingKeys,
        GetTickCount(), FileNameFromPath, createdPathsBySource);
}

void DesktopApp::StorePendingLandingCache(const DragSourceList& sourceList,
    const DropPreviewList& preview, const std::unordered_set<std::wstring>& existingKeys,
    const std::unordered_map<size_t, std::wstring>* createdPathsBySource)
{
    snowdesktop::pending_drop::Publish(pendingLandingCaches_,
        BuildPendingLandingCache(sourceList, preview, existingKeys, createdPathsBySource));
}

PendingFolderPlacement DesktopApp::BuildPendingFolderPlacement(
    const DesktopWidget& targetWidget,
    size_t insertIndex,
    const DragSourceList* sourceList) const
{
    const bool popup = targetWidget.id == kDockFolderPopupWidgetId;
    return snowdesktop::pending_drop::BuildFolder(targetWidget, insertIndex, sourceList,
        popup ? dockFolderPopupMappingWidgetId_ : targetWidget.id,
        popup ? dockFolderPopupSourceId_ : std::wstring{}, ToUpperInvariant, FileNameFromPath);
}

void DesktopApp::ActivatePendingFolderPlacement(PendingFolderPlacement placement)
{
    PendingLandingCache cache;
    cache.folderPlacements.push_back(std::move(placement));
    snowdesktop::pending_drop::Publish(pendingLandingCaches_, std::move(cache));
}

/**
 * @brief 执行基于文件系统的拖拽放置计划（复制/移动到桌面或文件夹映射）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
bool DesktopApp::ExecuteFileBackedDropPlan(const DragSourceList& sourceList,
    const DropPreviewList& preview, FileOperationCompletion completion,
    bool executeSynchronously,
    std::shared_ptr<snowdesktop::ShellFileOperationResult> result)
{
    snowdesktop::file_drop_execution::Services services;
    services.snapshotDesktopKeys = [this] { return SnapshotDesktopKeys(); };
    services.folderPlacement = [this](const auto& widget, size_t index, const DragSourceList* sources) {
        return BuildPendingFolderPlacement(widget, index, sources);
    };
    services.fileName = FileNameFromPath;
    services.pathsEqual = PathsEqualInsensitive;
    services.desktop = [this](const auto& sources, DropAction action, bool duplicate,
        auto done, bool sync, auto outputs) {
        return MaterializeFilesToDesktop(sources, action, duplicate, nullptr, std::move(done), sync, std::move(outputs));
    };
    services.folder = [this](const auto& sources, const auto& path, DropAction action,
        auto done, bool sync, auto outputs) {
        return MaterializeFilesToFolder(sources, path, action, std::move(done), sync, std::move(outputs));
    };
    services.pin = [this](const auto& paths, size_t index) { return AddMaterializedItemsToDock(paths, index); };
    services.savePins = [this] { SaveLayoutSlots(); };
    services.forgetMovedKeys = [this](const auto& keys) { RemoveDesktopKeysFromWidgets(keys); };
    services.refresh = [this] { RequestShellRefresh(); };
    return snowdesktop::file_drop_execution::Execute(sourceList, preview, pendingLandingCaches_,
        std::move(services), std::move(completion), executeSynchronously, std::move(result));
}
