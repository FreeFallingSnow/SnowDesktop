#include "app.h"
#include "../widgets/collection_group_rules.h"

// Drag source normalization and destination preview planning.

DragSourceList DesktopApp::BuildDragSourceList(
    const std::vector<Item*>& sourceItems, Container* origin) const
{
    DragSourceList list;
    list.BindRuntimeOrigin(origin);

    WidgetContainer* originWidget = dynamic_cast<WidgetContainer*>(origin);
    FileGroup* fileGroupOrigin =
        dynamic_cast<FileGroup*>(originWidget);
    const bool fileGroupLabelDrag =
        std::any_of(
            sourceItems.begin(), sourceItems.end(),
            [](Item* item) {
                return dynamic_cast<
                    FileGroupEntryItem*>(item) != nullptr;
            });
    if (fileGroupOrigin && !fileGroupLabelDrag)
    {
        ScrollingItemWidget* logicalSource =
            !sourceItems.empty()
                ? fileGroupOrigin->
                    GetSourceContainerForItem(
                        sourceItems.front())
                : nullptr;
        if (!logicalSource)
            logicalSource =
                fileGroupOrigin->
                    GetActiveSourceContainer();
        if (logicalSource)
        {
            originWidget = logicalSource;
            list.BindRuntimeOrigin(logicalSource);
        }
    }
    DesktopWidget* originData = originWidget ? originWidget->GetWidgetData() : nullptr;
    if (originData)
    {
        list.hasOriginWidget = true;
        list.originWidgetId = originData->id;
        list.originWidgetType = originData->type;
    }

    for (auto* src : sourceItems)
    {
        if (!src) continue;
        DragSourceEntry entry;
        entry.item = src;
        entry.sourceIndex = list.entries.size();
        entry.displayName = src->GetTitle();
        entry.filePath = src->GetPath();

        if (auto* dockItem = dynamic_cast<DockEntryItem*>(src))
        {
            entry.fromDock = true;
            entry.dockReference = dockItem->GetReference();
            entry.dockEntryType = dockItem->GetEntryType();
            if (entry.dockEntryType == DockEntryType::DesktopItem)
            {
                entry.kind = DropSourceKind::DesktopIcon;
                entry.desktopKey = entry.dockReference;
                entry.desktopIndex = FindItemIndexByKey(entry.desktopKey);
                list.hasDesktopIcons = true;
                if (entry.desktopIndex < items_.size())
                {
                    const DesktopItem& item = items_[entry.desktopIndex];
                    entry.filePath = item.parsingName;
                    entry.originalCell = item.gridCell;
                    entry.originalSpan = item.gridSpan;
                    entry.protectedDesktopIcon = IsProtectedDesktopIcon(item);
                }
            }
            else
            {
                entry.kind = DropSourceKind::Widget;
                list.hasWidgets = true;
                size_t widgetIndex = FindWidgetIndexById(entry.dockReference);
                if (widgetIndex < widgets_.size())
                {
                    entry.originalCell = widgets_[widgetIndex].gridCell;
                    entry.originalSpan = widgets_[widgetIndex].gridSpan;
                }
            }
        }
        else if (auto* frequentItem = dynamic_cast<DockFrequentItem*>(src))
        {
            entry.fromDock = true;
            entry.kind = DropSourceKind::DesktopIcon;
            entry.desktopIndex = frequentItem->GetItemIndex();
            list.hasDesktopIcons = true;
            if (entry.desktopIndex < items_.size())
            {
                const DesktopItem& item = items_[entry.desktopIndex];
                entry.desktopKey = item.layoutKey;
                entry.filePath = item.parsingName;
                entry.originalCell = item.gridCell;
                entry.originalSpan = item.gridSpan;
            }
        }
        else if (dynamic_cast<Widget*>(src))
        {
            entry.kind = DropSourceKind::Widget;
            list.hasWidgets = true;
        }
        else if (auto* groupEntry =
            dynamic_cast<CollectionGroupEntryItem*>(src))
        {
            entry.kind = DropSourceKind::CollectionGroupEntry;
            entry.widgetId = groupEntry->GetCollectionId();
            list.hasCollectionGroupEntries = true;
            const size_t widgetIndex =
                FindWidgetIndexById(entry.widgetId);
            if (widgetIndex < widgets_.size())
            {
                entry.originalCell = widgets_[widgetIndex].gridCell;
                entry.originalSpan = widgets_[widgetIndex].gridSpan;
            }
        }
        else if (auto* fileGroupEntry =
            dynamic_cast<FileGroupEntryItem*>(src))
        {
            list.hasFileGroupSourceLabels = true;
            entry.widgetId =
                fileGroupEntry->GetChildWidgetId();
            const size_t widgetIndex =
                FindWidgetIndexById(entry.widgetId);
            if (widgetIndex < widgets_.size())
            {
                entry.originalCell = widgets_[widgetIndex].gridCell;
                entry.originalSpan = widgets_[widgetIndex].gridSpan;
                if (widgets_[widgetIndex].type ==
                        DesktopWidgetType::FolderMapping)
                {
                    entry.kind = DropSourceKind::Widget;
                    entry.dockEntryType =
                        DockEntryType::FolderMapping;
                    list.hasWidgets = true;
                }
                else
                {
                    entry.kind =
                        DropSourceKind::FileGroupEntry;
                    list.hasFileGroupEntries = true;
                }
            }
        }
        else if (auto* icon = dynamic_cast<DesktopIcon*>(src))
        {
            entry.kind = DropSourceKind::DesktopIcon;
            list.hasDesktopIcons = true;
            if (originData &&
                originData->type == DesktopWidgetType::CollectionGroup)
            {
                auto* group =
                    dynamic_cast<CollectionGroup*>(
                        originWidget);
                entry.widgetId = group
                    ? group->GetActiveCollectionId()
                    : originData->activeCategoryId;
            }
            else if (fileGroupOrigin)
            {
                if (auto* logicalSource =
                        fileGroupOrigin->
                            GetSourceContainerForItem(src))
                {
                    if (DesktopWidget* logicalData =
                            logicalSource->
                                GetWidgetData())
                        entry.widgetId =
                            logicalData->id;
                }
            }
            if (DesktopItem* item = icon->GetDesktopItem())
            {
                entry.desktopKey = item->layoutKey;
                entry.desktopIndex = FindItemIndexByKey(item->layoutKey);
                entry.originalCell = item->gridCell;
                entry.originalSpan = item->gridSpan;
                entry.protectedDesktopIcon = IsProtectedDesktopIcon(*item);
                if (entry.filePath.empty() && !entry.protectedDesktopIcon)
                    entry.filePath = icon->GetPath();
            }
        }
        else if (dynamic_cast<FolderEntryIcon*>(src))
        {
            entry.kind = DropSourceKind::FolderEntry;
            list.hasFolderEntries = true;
            if (fileGroupOrigin)
            {
                if (auto* logicalSource =
                        fileGroupOrigin->
                            GetSourceContainerForItem(src))
                {
                    if (DesktopWidget* logicalData =
                            logicalSource->
                                GetWidgetData())
                        entry.widgetId =
                            logicalData->id;
                }
            }
        }
        else if (dynamic_cast<ExternalFileItem*>(src))
        {
            entry.kind = DropSourceKind::ExternalFile;
            list.hasExternalFiles = true;
        }

        if (originData)
        {
            if (originData->type == DesktopWidgetType::FolderMapping)
            {
                auto it = std::find_if(originData->folderEntries.begin(), originData->folderEntries.end(),
                    [&](const FolderEntry& folderEntry) {
                        return PathsEqualInsensitive(folderEntry.fullPath, entry.filePath);
                    });
                if (it != originData->folderEntries.end())
                    entry.memberIndex = static_cast<size_t>(std::distance(originData->folderEntries.begin(), it));
            }
            else if (originData->type ==
                DesktopWidgetType::CollectionGroup)
            {
                if (entry.kind ==
                        DropSourceKind::CollectionGroupEntry &&
                    !entry.widgetId.empty())
                {
                    auto it = std::find(
                        originData->childWidgetIds.begin(),
                        originData->childWidgetIds.end(),
                        entry.widgetId);
                    if (it != originData->childWidgetIds.end())
                        entry.memberIndex = static_cast<size_t>(
                            std::distance(
                                originData->childWidgetIds.begin(), it));
                }
                else if (entry.kind ==
                             DropSourceKind::DesktopIcon &&
                         !entry.widgetId.empty() &&
                         !entry.desktopKey.empty())
                {
                    const size_t childIndex =
                        FindWidgetIndexById(entry.widgetId);
                    if (childIndex < widgets_.size())
                    {
                        const auto& childKeys =
                            widgets_[childIndex].itemKeys;
                        auto it = std::find_if(
                            childKeys.begin(), childKeys.end(),
                            [&](const std::wstring& key) {
                                return ToUpperInvariant(key) ==
                                    ToUpperInvariant(entry.desktopKey);
                            });
                        if (it != childKeys.end())
                            entry.memberIndex =
                                static_cast<size_t>(
                                    std::distance(
                                        childKeys.begin(), it));
                    }
                }
            }
            else if (originData->type ==
                DesktopWidgetType::FileGroup)
            {
                if (entry.kind ==
                        DropSourceKind::FileGroupEntry &&
                    !entry.widgetId.empty())
                {
                    auto it = std::find(
                        originData->childWidgetIds.begin(),
                        originData->childWidgetIds.end(),
                        entry.widgetId);
                    if (it != originData->childWidgetIds.end())
                        entry.memberIndex =
                            static_cast<size_t>(
                                std::distance(
                                    originData->childWidgetIds.begin(),
                                    it));
                }
            }
            else if (!entry.desktopKey.empty())
            {
                auto it = std::find_if(originData->itemKeys.begin(), originData->itemKeys.end(),
                    [&](const std::wstring& key) {
                        return ToUpperInvariant(key) == ToUpperInvariant(entry.desktopKey);
                    });
                if (it != originData->itemKeys.end())
                    entry.memberIndex = static_cast<size_t>(std::distance(originData->itemKeys.begin(), it));
            }
        }

        if (entry.originalSpan.columns <= 0) entry.originalSpan.columns = 1;
        if (entry.originalSpan.rows <= 0) entry.originalSpan.rows = 1;
        list.entries.push_back(entry);
    }
    return list;
}

/**
 * @brief 判断拖拽操作是否需要文件系统支持（复制/链接到文件夹映射等场景）。
 * @param sourceList 拖拽源列表。
 * @param targetKind 目标类型。
 * @param action 拖拽动作。
 * @return 需要文件系统支持返回 true。
 */
bool DesktopApp::IsDropFileBacked(const DragSourceList& sourceList,
    DropTargetKind targetKind, DropAction action) const
{
    if (sourceList.Empty()) return false;
    if (targetKind == DropTargetKind::FolderMapping) return true;
    if (action == DropAction::Copy || action == DropAction::Link) return true;
    return sourceList.hasExternalFiles || sourceList.hasFolderEntries;
}

bool DesktopApp::IsAutoCollectFileCategorySource(
    const DragSourceList& sourceList) const
{
    if (!sourceList.hasOriginWidget ||
        sourceList.originWidgetType != DesktopWidgetType::FileCategories)
        return false;

    auto it = std::find_if(widgets_.begin(), widgets_.end(),
        [&](const DesktopWidget& widget) {
            return widget.id == sourceList.originWidgetId;
        });
    return it != widgets_.end() && it->autoCollect;
}

/**
 * @brief 构建拖拽预览列表，计算放置目标、动作和落点。
 * @param sourceList 拖拽源列表。
 * @param target 目标容器。
 * @param targetSlot 目标槽位。
 * @param region 命中区域。
 * @param mods 修饰键。
 * @param dropPoint 放置点坐标。
 * @return 拖拽预览列表。
 */
DropPreviewList DesktopApp::BuildDropPreviewList(const DragSourceList& sourceList,
    Container* target, Slot* targetSlot, HitRegion region, int mods, POINT dropPoint) const
{
    DropPreviewList preview;
    preview.targetContainer = target;
    if (sourceList.Empty() || !target || sourceList.hasWidgets || region == HitRegion::Handoff)
        return preview;
    if (!DragTargetResolver::AcceptsInternal(
            *target, sourceList))
        return preview;

    DropAction defaultAction = sourceList.hasExternalFiles ? DropAction::Copy : DropAction::Move;
    preview.action = DropActionFromMods(mods, defaultAction);

    if (!containers_.empty() && target == containers_.front().get())
    {
        preview.targetKind = DropTargetKind::Desktop;
        if (sourceList.hasCollectionGroupEntries ||
            sourceList.hasFileGroupEntries)
        {
            preview.action = DropAction::Move;
            preview.fileBacked = false;
            GridCell targetCell =
                CellFromPointForDrag(dropPoint);
            preview.anchorCell = targetCell;
            const GridPage* page =
                FindGridPage(gridPages_, targetCell.pageId);
            if (!page) return preview;

            std::unordered_set<std::wstring> usedSlots;
            for (const auto& widget : widgets_)
                if (!IsGroupedWidget(widget))
                    MarkGridArea(
                        usedSlots, widget.gridCell,
                        widget.gridSpan);
            for (const auto& item : items_)
                if (!item.name.empty() &&
                    !IsItemInAnyWidget(item))
                    MarkGridArea(
                        usedSlots, item.gridCell,
                        item.gridSpan);

            std::vector<const DragSourceEntry*>
                groupEntries;
            std::vector<
                snowdesktop::collection_group_rules::Span>
                requestedSpans;
            for (const auto& entry : sourceList.entries)
            {
                const bool matchingEntry =
                    sourceList.hasCollectionGroupEntries
                        ? entry.kind ==
                            DropSourceKind::CollectionGroupEntry
                        : entry.kind ==
                            DropSourceKind::FileGroupEntry;
                if (!matchingEntry)
                    continue;
                groupEntries.push_back(&entry);
                requestedSpans.push_back({
                    entry.originalSpan.columns,
                    entry.originalSpan.rows
                });
            }
            const auto placements =
                snowdesktop::collection_group_rules::
                    PlanExactPlacements(
                        page->columns, page->rows,
                        targetCell.column,
                        targetCell.row,
                        requestedSpans,
                        [&](const auto& placement) {
                            return AreGridSlotsMarked(
                                usedSlots,
                                {
                                    targetCell.pageId,
                                    placement.column,
                                    placement.row
                                },
                                {
                                    placement.span.columns,
                                    placement.span.rows
                                });
                        },
                        [&](const auto& placement) {
                            MarkGridArea(
                                usedSlots,
                                {
                                    targetCell.pageId,
                                    placement.column,
                                    placement.row
                                },
                                {
                                    placement.span.columns,
                                    placement.span.rows
                                });
                        });
            if (!placements)
                return preview;

            for (size_t i = 0;
                i < placements->size(); ++i)
            {
                const auto& placement =
                    (*placements)[i];
                DropLanding landing;
                landing.kind =
                    DropLandingKind::DesktopCell;
                landing.sourceIndex =
                    groupEntries[i]->sourceIndex;
                landing.cell = {
                    targetCell.pageId,
                    placement.column,
                    placement.row
                };
                landing.span = {
                    placement.span.columns,
                    placement.span.rows
                };
                preview.landings.push_back(landing);
            }
            return preview;
        }
        if (preview.action == DropAction::Move &&
            IsAutoCollectFileCategorySource(sourceList))
            return preview;

        const bool usePointerCell =
            sourceList.hasOriginWidget &&
            sourceList.originWidgetType ==
                DesktopWidgetType::CollectionGroup;
        POINT adjusted = !sourceList.origin || usePointerCell
            ? dropPoint
            : GetDragTargetPoint(dropPoint);
        GridCell targetCell = CellFromPointForDrag(adjusted);
        bool internalMove = !IsDropFileBacked(sourceList, preview.targetKind, preview.action);
        if (internalMove)
            targetCell = FindBestDropCell(targetCell);
        preview.anchorCell = targetCell;
        preview.fileBacked = !internalMove;
        preview.landings = BuildDesktopLandings(sourceList, targetCell, internalMove);
        return preview;
    }

    if (auto* widget = dynamic_cast<WidgetContainer*>(target))
    {
        preview.targetWidget = widget->GetWidgetData();
        if (sourceList.hasCollectionGroupEntries ||
            sourceList.hasFileGroupEntries)
        {
            const DesktopWidgetType expectedGroup =
                sourceList.hasCollectionGroupEntries
                    ? DesktopWidgetType::CollectionGroup
                    : DesktopWidgetType::FileGroup;
            if (!preview.targetWidget ||
                preview.targetWidget->type != expectedGroup)
                return preview;
            preview.targetKind = DropTargetKind::KeyedWidget;
            preview.action = DropAction::Move;
            preview.fileBacked = false;
            preview.insertIndex =
                widget->GetDropInsertIndex(targetSlot, region);
            const bool emptyGroupTabPoint =
                (dynamic_cast<CollectionGroup*>(widget) &&
                    dynamic_cast<CollectionGroup*>(widget)->
                        CategoryIdAtPoint(dropPoint).empty()) ||
                (dynamic_cast<FileGroup*>(widget) &&
                    dynamic_cast<FileGroup*>(widget)->
                        SourceIdAtPoint(dropPoint).empty());
            if (emptyGroupTabPoint)
                preview.insertIndex =
                    preview.targetWidget->childWidgetIds.size();
            for (const auto& entry : sourceList.entries)
            {
                const bool matchingEntry =
                    sourceList.hasCollectionGroupEntries
                        ? entry.kind ==
                            DropSourceKind::CollectionGroupEntry
                        : entry.kind ==
                            DropSourceKind::FileGroupEntry;
                if (!matchingEntry)
                    continue;
                DropLanding landing;
                landing.kind = DropLandingKind::WidgetIndex;
                landing.sourceIndex = entry.sourceIndex;
                landing.widget = preview.targetWidget;
                landing.widgetId = preview.targetWidget->id;
                landing.insertIndex =
                    preview.insertIndex + preview.landings.size();
                preview.landings.push_back(landing);
            }
            return preview;
        }
        if (preview.targetWidget &&
            preview.targetWidget->type ==
                DesktopWidgetType::CollectionGroup)
        {
            auto* group =
                dynamic_cast<CollectionGroup*>(widget);
            std::wstring activeId = group
                ? group->CategoryIdAtPoint(dropPoint)
                : L"";
            if (activeId.empty() && group)
                activeId = group->GetActiveCollectionId();
            const size_t activeIndex =
                FindWidgetIndexById(activeId);
            if (activeIndex >= widgets_.size() ||
                widgets_[activeIndex].type !=
                    DesktopWidgetType::Collection)
                return preview;
            // 集合组只是当前集合的可视代理。普通图标拖放仍落到
            // 激活标签对应的 Collection 数据中。
            preview.targetWidget = const_cast<DesktopWidget*>(
                &widgets_[activeIndex]);
        }
        if (preview.targetWidget &&
            preview.targetWidget->type ==
                DesktopWidgetType::FileGroup)
        {
            auto* group = dynamic_cast<FileGroup*>(widget);
            std::wstring activeId = group
                ? group->GetActiveSourceId()
                : preview.targetWidget->activeCategoryId;
            const size_t activeIndex =
                FindWidgetIndexById(activeId);
            if (activeIndex >= widgets_.size() ||
                (widgets_[activeIndex].type !=
                    DesktopWidgetType::FileCategories &&
                 widgets_[activeIndex].type !=
                    DesktopWidgetType::FolderMapping))
                return preview;
            preview.targetWidget =
                const_cast<DesktopWidget*>(
                    &widgets_[activeIndex]);
        }
        if (preview.targetWidget &&
            preview.targetWidget->type == DesktopWidgetType::FileCategories)
        {
            auto isShortcutPath = [](const std::wstring& path) {
                return !path.empty() && _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
            };
            bool sourceHasShortcut = std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) {
                    return isShortcutPath(entry.filePath) || isShortcutPath(entry.displayName);
                });
            if (preview.action == DropAction::Link || sourceHasShortcut)
            {
                preview.targetKind = DropTargetKind::KeyedWidget;
                return preview;
            }
        }
        preview.targetKind = preview.targetWidget &&
            preview.targetWidget->type == DesktopWidgetType::FolderMapping
                ? DropTargetKind::FolderMapping
                : DropTargetKind::KeyedWidget;
        // 文件夹拖入自身（或自身子目录）的映射/弹窗：不生成落点计划，
        // 执行层也会拦截，避免把文件夹递归复制进自己。
        if (preview.targetKind == DropTargetKind::FolderMapping &&
            preview.targetWidget &&
            IsSelfContainedFolderDrop(
                sourceList.FilePaths(),
                preview.targetWidget->sourceFolderPath))
            return preview;
        const bool sourceFromDock = std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
            [](const DragSourceEntry& entry) { return entry.fromDock; });
        if (preview.targetKind == DropTargetKind::FolderMapping && sourceFromDock &&
            preview.action == DropAction::Move)
        {
            // Dock entries are layout references. A logical move out of Dock
            // must not physically remove the backing desktop file/shortcut.
            preview.action = DropAction::Copy;
            preview.consumeDockSource = true;
        }
        Container* resolvedTarget = target;
        if (auto* group = dynamic_cast<FileGroup*>(widget))
            if (auto* active = group->GetActiveSourceContainer())
                resolvedTarget = active;
        if (auto* group =
                dynamic_cast<FileGroup*>(widget);
            group && group->GetWidgetData() &&
            group->GetWidgetData()->dateHeaders &&
            sourceList.origin == resolvedTarget &&
            preview.action == DropAction::Move)
            return preview;
        if (preview.targetWidget &&
            preview.targetWidget->dateHeaders &&
            sourceList.origin == resolvedTarget &&
            preview.action == DropAction::Move)
            return preview;
        preview.fileBacked = !(
            sourceList.origin == resolvedTarget &&
            preview.action == DropAction::Move) &&
            IsDropFileBacked(sourceList, preview.targetKind, preview.action);
        preview.insertIndex = widget->GetDropInsertIndex(targetSlot, region);
        if (auto* group =
                dynamic_cast<CollectionGroup*>(widget);
            group && !group->CategoryIdAtPoint(dropPoint).empty() &&
            preview.targetWidget)
            preview.insertIndex =
                preview.targetWidget->itemKeys.size();
        for (const auto& entry : sourceList.entries)
        {
            DropLanding landing;
            landing.kind = preview.targetKind == DropTargetKind::FolderMapping
                ? DropLandingKind::Folder
                : DropLandingKind::WidgetIndex;
            landing.sourceIndex = entry.sourceIndex;
            landing.widget = preview.targetWidget;
            if (preview.targetWidget)
                landing.widgetId = preview.targetWidget->id;
            landing.insertIndex = preview.insertIndex + preview.landings.size();
            if (preview.targetWidget)
                landing.cell = preview.targetWidget->gridCell;
            preview.landings.push_back(landing);
        }
        return preview;
    }

    return preview;
}

/**
 * @brief 构建外部文件拖入桌面的放置预览列表。
 * @param targetCell 目标网格单元格。
 * @param count 外部文件数量。
 * @return 拖拽预览列表。
 */
DropPreviewList DesktopApp::BuildExternalDesktopPreviewList(GridCell targetCell, size_t count) const
{
    DragSourceList list;
    list.hasExternalFiles = true;
    for (size_t i = 0; i < count; ++i)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = i;
        entry.originalSpan = {1, 1};
        list.entries.push_back(entry);
    }

    DropPreviewList preview;
    preview.targetKind = DropTargetKind::Desktop;
    preview.action = DropAction::Copy;
    preview.fileBacked = true;
    preview.anchorCell = targetCell;
    preview.landings = BuildDesktopLandings(list, targetCell, false);
    return preview;
}

/**
 * @brief 执行拖拽管线的完整流程（文件落地或内部移动）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
