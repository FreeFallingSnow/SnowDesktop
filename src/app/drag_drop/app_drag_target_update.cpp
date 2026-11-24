#include "app/app.h"
#include "layout/collection_popup_layout.h"
#include "layout/folder_preview_header_layout.h"
#include "common/l10n.h"
#include "common/utils.h"
#include "core/drag_target_resolver.h"
#include "layout/popup_drag_rules.h"
#include "layout/page_navigation_rules.h"
#include "widget/runtime/widget_item_layout.h"
#include "core/drag_source_rebind.h"
#include "core/transient_drag_slot.h"
#include "drag_drop/file_source_eligibility.h"
#include <map>
#include <tuple>

// Drag-target resolution, popup hit testing and page-navigation dwell.

void DesktopApp::ResolveCurrentDragTargetAt(POINT clientPoint)
{
    if (dockRunningDrag_ && dragSession_.IsActive())
    {
        UpdateDockRunningDrag(clientPoint);
        return;
    }
    if (HitTestLuaFileDropTarget(clientPoint) < widgets_.size())
    {
        dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        return;
    }
    if (!dragSession_.IsActive()) return;

    dragSession_.UpdatePoint(clientPoint);

    const bool external = dragDropController_.IsExternalDragActive();
    const auto& sources = dragSession_.SourceList();
    const bool suppressDesktopWidgetTargets = !external && SuppressDesktopWidgetDragTargets();
    bool popupHit = false;
    const auto resolve = [&]() -> DragTargetResolution {
        DragTargetResolution target;
        popupHit = !suppressDesktopWidgetTargets &&
            (external || !(sources.hasCollectionGroupEntries || sources.hasFileGroupEntries) || GetGroupPopupView()) &&
            HitTestPopupForDrag(clientPoint, target.container, target.slot, target.region);
        if (popupHit) return target;

        const auto visible = [&](const Container& candidate) {
            return !desktopIconsHidden_ || IsRetainedContainer(&candidate);
        };
        if (external) return dragDropController_.ResolveExternalTarget(containers_, clientPoint, visible);
        return dragDropController_.ResolveInternalTarget(containers_, clientPoint, [&](const Container& candidate) {
            return visible(candidate) && (!suppressDesktopWidgetTargets ||
                (!dynamic_cast<const DesktopGrid*>(&candidate) && !dynamic_cast<const WidgetContainer*>(&candidate)));
        });
    };
    auto target = resolve();

    if (!external && snowdesktop::file_source_eligibility::ContainsProtectedDesktopIcons(sources) &&
        snowdesktop::file_source_eligibility::ForPhysicalFolder(sources).Empty())
    {
        auto* widget = dynamic_cast<WidgetContainer*>(target.container);
        const DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
        if (auto* group = dynamic_cast<FileGroup*>(widget))
        {
            auto id = group->SourceIdAtPoint(clientPoint);
            if (id.empty() && group->IsGroupSearchActive() && target.slot)
                if (auto* source = group->GetSourceContainerForItem(target.slot->GetItem()))
                    if (auto* sourceData = source->GetWidgetData()) id = sourceData->id;
            if (id.empty()) id = group->GetActiveSourceId();
            const auto index = FindWidgetIndexById(id);
            data = index < widgets_.size() ? &widgets_[index] : nullptr;
        }
        if (data && data->type == DesktopWidgetType::FolderMapping)
        {
            target.slot = nullptr;
            target.region = HitRegion::Blocked;
        }
    }

    const auto revealAll = [&] {
        if (target.region != HitRegion::Empty && target.region != HitRegion::SortBefore &&
            target.region != HitRegion::SortAfter) return false;
        auto* view = dynamic_cast<ScrollingItemWidget*>(target.container);
        if (!view || view->IsSearchActive()) return false;
        auto* group = dynamic_cast<FileGroup*>(view);
        auto* source = group ? group->GetActiveSourceContainer() : view;
        const auto* data = source ? source->GetWidgetData() : nullptr;
        if (!data || !data->showFileCategories || data->activeCategoryId == L"all" ||
            (data->type != DesktopWidgetType::FileCategories && data->type != DesktopWidgetType::FolderMapping))
            return false;
        if (popupHit && !group)
        {
            // The popup hit test already limits placement to its item viewport.
            if (GetCategorizedPopupView() != view) return false;
        }
        else
        {
            if (view->IsSingleCellDesktopPoint(clientPoint)) return false;
            const RECT viewport = view->GetContentViewportRect();
            if (clientPoint.y < viewport.top || clientPoint.y >= viewport.bottom) return false;
        }
        if (external)
        {
            const auto& summary = dragDropController_.ExternalSummary();
            if (summary.applicationShortcut ||
                !DragTargetResolver::AcceptsExternal(*view, static_cast<size_t>(std::max(1, summary.fileCount))) ||
                (data->type == DesktopWidgetType::FolderMapping &&
                 IsSelfContainedFolderDrop(externalFileDropPaths_, data->sourceFolderPath)))
                return false;
            return view->RevealAllForMemberDrop(sources, true);
        }

        const auto plan = BuildDropPreviewList(sources, view, target.slot, target.region,
            DropActionToMods(dragSession_.Action()), clientPoint);
        return !plan.Empty() && plan.targetContainer == source &&
            view->RevealAllForMemberDrop(sources, plan.fileBacked);
    };
    if (revealAll())
    {
        const auto* switched = target.container;
        dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        if (popupHit)
        {
            popupScrollOffset_ = 0;
            InvalidateCollectionPopupContent();
            RefreshOpenCollectionPopupGeometry();
        }
        InvalidateDragStaticScene();
        RequestLayoutSave();
        InvalidateRect(hwnd_, nullptr, FALSE);
        target = resolve();
        if (target.container == switched && target.region == HitRegion::Handoff && target.slot)
            dragSession_.KeepPlacementAfterViewChange(target.container, target.slot->GetBounds());
    }

    if (target.region == HitRegion::Handoff && target.slot &&
        dragSession_.KeepsPlacementAfterViewChange(target.container))
    {
        const RECT bounds = target.slot->GetBounds();
        const auto* popup = popupHit ? GetOpenPopupWidget() : nullptr;
        const bool vertical = popup && !IsGroupWidgetType(popup->type)
            ? UsesCollectionPopupList(*popup) && GetCollectionPopupColumnCount(GetCollectionPopupRect(*popup)) == 1
            : target.container->GetInsertionStyle() == BarStyle::HBar;
        target.region = snowdesktop::popup_drag_rules::IsAfterInsertionMidpoint(bounds, clientPoint, vertical)
            ? HitRegion::SortAfter : HitRegion::SortBefore;
    }
    dragSession_.UpdateTarget(target.container, target.slot, target.region);
}

void DesktopApp::RefreshDragTargetAt(POINT clientPoint, int mods)
{
    if (!dragSession_.IsActive()) return;

    if (!dragDropController_.IsExternalDragActive())
        dragSession_.UpdateActionFromMods(mods);
    ResolveCurrentDragTargetAt(clientPoint);

    if (!UpdateWidgetPairDragHint(clientPoint, mods))
        ShowDragHintWindow(clientPoint, GetCurrentDragTargetHint(mods));
    InvalidateFloatingDockWindow(true);
}

std::wstring DesktopApp::GetCurrentDragTargetHint(
    int mods, std::optional<DWORD> negotiatedEffect) const
{
    const auto* target = dragSession_.TargetContainer();
    const auto region = dragSession_.TargetRegion();
    if (!target || region == HitRegion::None || region == HitRegion::Blocked) return {};
    auto* slot = dragSession_.TargetSlot();
    if (region == HitRegion::Handoff)
    {
        const auto* item = slot ? slot->GetItem() : nullptr;
        const bool desktopTarget = dynamic_cast<const DesktopGrid*>(target) != nullptr;
        if (!item && desktopTarget)
            item = HitTestIcon(dragSession_.CurrentPoint());
        const DesktopItem* desktop = nullptr;
        bool directory = false;
        if (const auto* icon = dynamic_cast<const DesktopIcon*>(item))
            desktop = icon->GetDesktopItem();
        else if (const auto* frequent = dynamic_cast<const DockFrequentItem*>(item))
        {
            const auto index = frequent->GetItemIndex();
            if (index < items_.size()) desktop = &items_[index];
        }
        else if (const auto* folderIcon = dynamic_cast<const FolderEntryIcon*>(item))
        {
            if (const auto* entry = folderIcon->GetFolderEntry())
                directory = entry->isDirectory || entry->shortcutTarget.directory;
        }
        else if (const auto* dockItem = dynamic_cast<const DockEntryItem*>(item))
        {
            const auto index = dockItem->GetEntryIndex();
            if (index < dockEntries_.size())
                directory = ResolveDockFolderTarget(dockEntries_[index]).available;
        }
        else if (folderPreview_ && slot == popupDragTarget_.Get())
            directory = dynamic_cast<const ExternalFileItem*>(item) != nullptr;

        const bool canCopyLargeIconSettings = desktopTarget && desktop && !rightButtonItemDrag_ &&
            (mods & (MK_SHIFT | MK_ALT)) == 0 &&
            FindLargeIconSettingsDragSource(dragSession_.SourceList(), *desktop);
        if (canCopyLargeIconSettings && (mods & MK_CONTROL))
            return _LFW("core.drag.release_copy_large_icon_settings", desktop->name);
        const auto withSettingsHint = [canCopyLargeIconSettings](std::wstring hint) {
            return canCopyLargeIconSettings && !hint.empty()
                ? _LFW("core.drag.with_large_icon_settings_hint", hint) : hint;
        };
        if (desktop && desktop->desktopIconClsid.empty())
        {
            // Filesystem enumeration records a timestamp for both types and
            // a size only for files. Link metadata identifies directory links.
            directory = desktop->shortcutTarget.directory ||
                (desktop->modifiedTime.has_value() && !desktop->fileSize.has_value());
        }
        if (directory)
        {
            DWORD effect = negotiatedEffect.value_or(DROPEFFECT_NONE);
            if (!negotiatedEffect)
            {
                const DWORD allowed = dragDropController_.IsExternalDragActive()
                    ? dragDropController_.ExternalSummary().allowedEffects
                    : DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                effect = dragDropController_.IsExternalDragActive() && externalOwnedContent_
                    ? allowed & DROPEFFECT_COPY
                    : ChooseDropEffect(static_cast<DWORD>(mods), allowed);
            }
            auto name = item->GetTitle();
            if (name.empty()) name = item->GetPath();
            switch (effect)
            {
            case DROPEFFECT_MOVE: return withSettingsHint(_LFW("core.drag.move_into_folder", name));
            case DROPEFFECT_COPY: return withSettingsHint(_LFW("core.drag.copy_into_folder", name));
            case DROPEFFECT_LINK: return withSettingsHint(_LFW("core.drag.link_in_folder", name));
            default: return {};
            }
        }
        return withSettingsHint(target->GetDragHint(slot, region, dragSession_.Items(), dragSession_.Source(), mods));
    }
    return target->GetDragHint(slot, region, dragSession_.Items(), dragSession_.Source(), mods);
}

/**
 * @brief 在重建容器后重新绑定拖拽源
 * @note 用于在容器重建后恢复拖拽会话的源引用
 */
void DesktopApp::RebindDragSourceAfterRebuild()
{
    if (!dragSession_.IsActive()) return;

    Container* source = nullptr;
    FileGroup* sourceFileGroup = nullptr;
    const DragSourceList& oldSourceList = dragSession_.SourceList();
    // External OLE drags do not have an internal source. Keep the session active
    // with empty source bindings; the next DragOver will rebuild its target.
    if (oldSourceList.Empty()) return;

    if (oldSourceList.hasSearchResults)
    {
        DragSourceList rebound = oldSourceList;
        for (const auto& container : containers_)
            if (auto* widget = dynamic_cast<WidgetContainer*>(container.get()); widget &&
                widget->GetWidgetData()->id == oldSourceList.searchViewId) { source = widget; break; }
        if (!source) { EndDragSession(); return; }
        std::vector<Item*> retained;
        for (auto& entry : rebound.entries)
        {
            if (!entry.retainedItem) { EndDragSession(); return; }
            entry.item = entry.retainedItem.get();
            if (!entry.desktopKey.empty()) entry.desktopIndex = FindItemIndexByKey(entry.desktopKey);
            retained.push_back(entry.item);
        }
        rebound.BindRuntimeOrigin(source);
        dragSession_.RebindSource(source, std::move(retained), std::move(rebound));
        return;
    }

    if (oldSourceList.SourceSurfaceKind() ==
        snowdesktop::slot_contract::SlotSurfaceKind::Dock)
    {
        // All monitor Docks share the same entries. Prefer the source display,
        // not the display currently under the pointer after a cross-screen drag.
        const GridPage* sourcePage = GridPageFromPoint(
            { dragGroupOriginX_, dragGroupOriginY_ });
        DockContainer* dock = nullptr;
        for (auto& container : containers_)
        {
            auto* candidate = dynamic_cast<DockContainer*>(container.get());
            if (!candidate) continue;
            if (!dock) dock = candidate;
            const RECT bounds = candidate->GetBounds();
            const GridPage* page = GridPageFromPoint(
                { bounds.left, bounds.top });
            if (sourcePage && page && page->monitorId == sourcePage->monitorId)
            {
                dock = candidate;
                break;
            }
        }
        auto reboundItems = snowdesktop::drag_source_rebind::
            ResolveRecordedDockItems(oldSourceList,
                [&](const DragSourceEntry& entry) -> Item* {
                    if (!dock) return nullptr;
                    for (const auto& slot : dock->GetSlots())
                    {
                        auto* item = slot->GetItem();
                        auto* dockItem = dynamic_cast<DockEntryItem*>(item);
                        if (dockItem && !entry.dockReference.empty() &&
                            dockItem->GetReference() == entry.dockReference &&
                            dockItem->GetEntryType() == entry.dockEntryType)
                            return item;
                        auto* frequent = dynamic_cast<DockFrequentItem*>(item);
                        if (frequent && entry.dockReference.empty() && !entry.desktopKey.empty() &&
                            frequent->GetItemIndex() == FindItemIndexByKey(entry.desktopKey))
                            return item;
                    }
                    return nullptr;
                });
        if (reboundItems.empty())
        {
            EndDragSession();
            return;
        }
        DragSourceList reboundList = oldSourceList;
        reboundList.BindRuntimeOrigin(dock);
        for (size_t i = 0; i < reboundItems.size(); ++i)
        {
            reboundList.entries[i].item = reboundItems[i];
            if (!reboundList.entries[i].desktopKey.empty())
                reboundList.entries[i].desktopIndex = FindItemIndexByKey(reboundList.entries[i].desktopKey);
        }
        dragSession_.RebindSource(dock, std::move(reboundItems),
            std::move(reboundList));
        return;
    }

    if (oldSourceList.hasOriginWidget)
    {
        for (auto& c : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(c.get());
            DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
            if (data && data->id == oldSourceList.originWidgetId)
            {
                source = widget;
                break;
            }
        }
        const size_t fileGroupIndex =
            FindFileGroupIndexForChild(
                oldSourceList.originWidgetId);
        if (fileGroupIndex < widgets_.size())
        {
            for (auto& c : containers_)
            {
                auto* group =
                    dynamic_cast<FileGroup*>(c.get());
                if (group &&
                    group->GetWidgetData() ==
                        &widgets_[fileGroupIndex])
                {
                    source = group->
                        GetSourceContainerById(
                            oldSourceList.originWidgetId);
                    sourceFileGroup = group;
                    break;
                }
            }
        }
    }
    else
    {
        source = GetDesktopGrid();
    }

    if (!source)
    {
        EndDragSession();
        return;
    }

    std::vector<Item*> reboundItems =
        sourceFileGroup
            ? sourceFileGroup->
                GetHostedSelectedItemsForSource(
                    oldSourceList.originWidgetId)
            : source->GetSelectedItems();
    if (oldSourceList.HasDesktopWidgetIcons())
    {
        reboundItems.clear();
        for (const auto& entry : oldSourceList.entries)
        {
            Item* item = nullptr;
            if (entry.desktopWidgetIcon)
            {
                for (const auto& container : containers_)
                {
                    auto* widget = dynamic_cast<WidgetContainer*>(container.get());
                    if (widget && widget->GetWidgetData() &&
                        widget->GetWidgetData()->id == entry.widgetId)
                    {
                        item = widget;
                        break;
                    }
                }
            }
            else for (const auto& object : items_oo_)
            {
                auto* icon = dynamic_cast<DesktopIcon*>(object.get());
                if (icon && icon->GetDesktopItem() &&
                    PathsEqualInsensitive(icon->GetDesktopItem()->layoutKey, entry.desktopKey))
                {
                    item = icon;
                    break;
                }
            }
            if (!item)
            {
                EndDragSession();
                return;
            }
            reboundItems.push_back(item);
        }
    }

    // When the page turn replaces the page on the same physical monitor,
    // the source widget is rebuilt after its model bounds become empty.  A
    // Collection/FolderMapping normally materializes drag items from visible
    // slots, so there are no selected runtime wrappers to rebind even though
    // the stable source entries still identify the exact members that began
    // the drag.  Recreate only those recorded members; do not broaden the drag
    // to other selected items that happened to be hidden at drag start.
    auto* widgetSource =
        dynamic_cast<WidgetContainer*>(source);
    reboundItems = snowdesktop::drag_source_rebind::
        ResolveItemsAfterRebuild(
            std::move(reboundItems),
            oldSourceList,
            [&](size_t memberIndex) -> Item* {
                return widgetSource
                    ? widgetSource->
                        GetMemberItem(memberIndex)
                    : nullptr;
            });
    // Selection can change while a menu or reload pumps messages. It is a UI
    // state, not a new drag payload. Resolve only the recorded identities, in
    // their original order, and never substitute a newly selected member.
    const auto identity = [](const DragSourceEntry& entry) {
        return std::tuple{entry.kind, entry.fromDock, entry.desktopWidgetIcon,
            entry.protectedDesktopIcon, entry.widgetType, entry.dockEntryType,
            entry.widgetId, ToUpperInvariant(entry.desktopKey),
            ToUpperInvariant(entry.filePath), ToUpperInvariant(entry.dockReference)};
    };
    using Identity = decltype(identity(oldSourceList.entries.front()));
    const auto resolveRecorded = [&](const std::vector<Item*>& candidates) {
        std::vector<Item*> resolved;
        const auto current = BuildDragSourceList(candidates, source);
        if (current.SourceSurfaceKind() != oldSourceList.SourceSurfaceKind() ||
            current.hasOriginWidget != oldSourceList.hasOriginWidget ||
            (current.hasOriginWidget && (current.originWidgetId != oldSourceList.originWidgetId ||
                current.originWidgetType != oldSourceList.originWidgetType))) return resolved;
        std::map<Identity, Item*> live;
        for (const auto& entry : current.entries)
            if (entry.item && (!entry.desktopKey.empty() || !entry.filePath.empty() ||
                    !entry.widgetId.empty() || !entry.dockReference.empty()))
                live.emplace(identity(entry), entry.item);
        std::unordered_set<Item*> used;
        for (const auto& entry : oldSourceList.entries)
        {
            const auto found = live.find(identity(entry));
            if (found == live.end() || !used.insert(found->second).second) return std::vector<Item*>{};
            resolved.push_back(found->second);
        }
        return resolved;
    };
    auto recordedItems = resolveRecorded(reboundItems);
    if (recordedItems.empty())
    {
        // An original member may no longer be highlighted but still be a live
        // member of the same source view. Query its current wrappers without
        // changing selection. Do not retain selected-wrapper pointers across
        // the lazy slot build, which may replace their cache.
        std::vector<Item*> visible;
        std::unordered_set<Item*> added;
        for (const auto& slot : source->GetSlots())
            if (auto* item = slot->GetItem(); item && added.insert(item).second) visible.push_back(item);
        for (const auto& object : items_oo_)
            if (object->GetContainer() == source && added.insert(object.get()).second) visible.push_back(object.get());
        recordedItems = resolveRecorded(visible);
    }
    if (recordedItems.empty())
    {
        EndDragSession();
        return;
    }
    DragSourceList reboundList = BuildDragSourceList(recordedItems, source);
    for (size_t i = 0; i < reboundList.entries.size(); ++i)
        reboundList.entries[i].sourceIndex = oldSourceList.entries[i].sourceIndex;
    dragSession_.RebindSource(source, std::move(recordedItems), std::move(reboundList));
}

/**
 * @brief 拖拽时优先检查集合弹窗命中（弹窗遮挡的容器不应被穿透命中）。
 * @param client 客户端坐标
 * @param[out] targetContainer 命中的容器
 * @param[out] targetSlot 命中的槽位
 * @param[out] targetRegion 命中的区域
 * @return 命中弹窗返回 true，否则 false
 */
bool DesktopApp::HitTestPopupForDrag(POINT client,
    Container*& targetContainer, Slot*& targetSlot, HitRegion& targetRegion)
{
    if (!inactiveInlineFolderPreviews_.empty() && !IsPointOccludedByOpenPopup(client) &&
        FindInactiveInlineFolderPreviewHostAtPoint(client))
    {
        ActivateInlineFolderPreviewAtPoint(client, true);
        if (!IsPointOccludedByOpenPopup(client))
        {
            // An inactive preview must not hand a drop to the hidden homepage
            // beneath it when a separate popup owns the active interaction.
            targetRegion = HitRegion::Blocked;
            return true;
        }
    }
    if (!IsCollectionPopupInteractive())
        return false;
    if (desktopIconsHidden_ && !IsOpenPopupRetained())
        return false;

    if (auto* group = GetGroupPopupView())
    {
        const RECT popup = GetCollectionPopupRect(*group->GetWidgetData());
        if (!PtInRect(&popup, client)) return false;
        const RECT frame = GetCategorizedPopupFrame(popup);
        group->SetPopupFrame(&frame);
        targetContainer = group;
        targetSlot = nullptr;
        if (!dragDropController_.IsExternalDragActive() &&
            !DragTargetResolver::AcceptsInternal(*group, dragSession_.SourceList()))
        {
            targetRegion = HitRegion::Blocked;
            return true;
        }
        targetRegion = group->HitTestDrag(client, targetSlot);
        return true;
    }

    if (dockFolderPopupOpen_ &&
        dockFolderPopupContainer_)
    {
        const RECT popup = GetCollectionPopupRect(
            dockFolderPopupWidget_);
        if (folderPreview_)
        {
            const auto* motion = GetCollectionPopupResizeTransition();
            const RECT panel = motion ? motion->Panel(popup) : popup;
            const auto navigation = GetFolderPreviewNavigationRow(panel);
            if (PtInRect(&navigation.breadcrumb, client))
            {
                targetContainer = dockFolderPopupContainer_.get();
                targetSlot = nullptr;
                targetRegion = HitRegion::Blocked;
                if (!dockFolderPopupAvailable_) return true;

                const bool external = dragDropController_.IsExternalDragActive();
                const auto sourcePaths = external ? externalFileDropPaths_ : dragSession_.SourceList().FilePaths();
                if (!external && sourcePaths.empty()) return true;
                for (const auto& crumb : GetFolderPreviewBreadcrumbs(panel))
                {
                    if (crumb.path.empty() || !PtInRect(&crumb.bounds, client)) continue;
                    if (IsSelfContainedFolderDrop(sourcePaths, crumb.path)) return true;

                    // Each ancestor has a distinct path length within this
                    // view, including when overflow changes the visible list.
                    targetSlot = popupDragTarget_.BindHandoff(targetContainer, crumb.bounds,
                        crumb.path.size(), SlotFeedbackRole::Popup, &dockFolderPopupWidget_,
                        [&] { return std::make_unique<ExternalFileItem>(crumb.path); });
                    targetRegion = HitRegion::Handoff;
                    return true;
                }
                return true;
            }
        }
        if (!PtInRect(&popup, client))
            return false;

        // 文件夹不能拖入自身弹窗：目标即源文件夹本身或其子目录，
        // 直接整窗标记 Blocked，避免递归复制进自己并清掉 Dock 引用。
        if (IsSelfContainedFolderDrop(
                dragSession_.SourceList().FilePaths(),
                dockFolderPopupWidget_.sourceFolderPath))
        {
            targetContainer =
                dockFolderPopupContainer_.get();
            targetSlot = nullptr;
            targetRegion = HitRegion::Blocked;
            return true;
        }

        const RECT content =
            GetCollectionPopupContentRect(popup);
        targetContainer =
            dockFolderPopupContainer_.get();
        targetSlot = nullptr;
        targetRegion = HitRegion::None;
        // Side padding belongs to the nearest insertion edge. Keep headers,
        // controls and the bottom chrome outside the item viewport.
        if (client.y < content.top || client.y >= content.bottom)
            return true;

        if (dockFolderPopupWidget_.
                folderEntries.empty())
        {
            RECT virtualItem =
                GetCollectionPopupItemRect(
                    popup, 0);
            RECT visibleItem{};
            if (!IntersectRect(
                    &visibleItem,
                    &virtualItem,
                    &content))
                visibleItem = content;
            targetSlot = popupDragTarget_.BindPlacement(
                targetContainer,
                visibleItem, 0, SlotFeedbackRole::Popup);
            targetRegion =
                HitRegion::SortBefore;
            return true;
        }

        for (size_t i = 0;
             i < GetPopupItemCount(dockFolderPopupWidget_); ++i)
        {
            RECT itemRect =
                GetCollectionPopupItemRect(popup, i);
            RECT clipped{};
            if (!IntersectRect(
                    &clipped, &itemRect, &content) ||
                (!HitTestCollectionPopupItem(popup, i, client) &&
                 !HitTestCollectionPopupHandoff(popup, i, client)))
                continue;

            FolderEntry& entry =
                dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(dockFolderPopupWidget_, i)];
            if (snowdesktop::popup_drag_rules::
                    CanHandoffToItem(
                        true, entry.selected) &&
                HitTestCollectionPopupHandoff(popup, i, client))
            {
                targetSlot = popupDragTarget_.BindHandoff(
                    targetContainer,
                    snowdesktop::popup_drag_rules::
                        HandoffIndicatorBounds(itemRect),
                    i, SlotFeedbackRole::Popup,
                    &entry,
                    [&]() -> std::unique_ptr<Item> {
                        return std::make_unique<
                            FolderEntryIcon>(
                                &entry,
                                dockFolderPopupContainer_.get(),
                                this);
                    });
                targetRegion = HitRegion::Handoff;
                return true;
            }

            targetSlot = popupDragTarget_.BindPlacement(
                targetContainer,
                itemRect, i, SlotFeedbackRole::Popup);
            targetRegion =
                snowdesktop::popup_drag_rules::
                    IsAfterInsertionMidpoint(
                        itemRect, client,
                        ((UsesCollectionPopupList(dockFolderPopupWidget_) && GetCollectionPopupColumnCount(popup) == 1) || UsesCollectionPopupFan(dockFolderPopupWidget_)))
                ? HitRegion::SortAfter
                : HitRegion::SortBefore;
            if (UsesCollectionPopupFan(dockFolderPopupWidget_))
                targetRegion = snowdesktop::collection_popup_layout::FanIsAfterInsertion(
                    GetCollectionPopupFanItem(popup, i), client, CollectionPopupFanRootAbove())
                    ? HitRegion::SortAfter : HitRegion::SortBefore;
            return true;
        }

        long long bestDistanceSquared =
            std::numeric_limits<
                long long>::max();
        size_t nearestIndex = 0;
        RECT nearestBounds{};
        HitRegion nearestRegion =
            HitRegion::SortAfter;
        for (size_t i = 0;
            i < GetPopupItemCount(dockFolderPopupWidget_); ++i)
        {
            const RECT itemRect =
                GetCollectionPopupItemRect(
                    popup, i);
            RECT clipped{};
            if (!IntersectRect(
                    &clipped, &itemRect,
                    &content))
                continue;
            const HitRegion edgeRegions[] = {
                HitRegion::SortBefore,
                HitRegion::SortAfter,
            };
            for (size_t edge = 0;
                edge < 2; ++edge)
            {
                const bool after = edge != 0;
                const auto popupMetrics =
                    GetOpenCollectionPopupLayoutMetrics();
                const bool listLayout = UsesCollectionPopupList(dockFolderPopupWidget_);
                const long gutter = folderPreview_
                    ? static_cast<long>(std::ceil(GetFolderPreviewInsertionPad(popup, i, after)))
                    : listLayout
                    ? static_cast<long>(snowdesktop::widget_item_layout::InsertionBoundaryPad(
                        GetCollectionPopupItemRect(popup, 0), GetCollectionPopupItemRect(popup, 1),
                        GetCollectionPopupColumnCount(popup) > 1))
                    : UsesCollectionPopupFan(dockFolderPopupWidget_) ? popupMetrics.gapY / 2 : popupMetrics.gapX / 2;
                const long long distance = UsesCollectionPopupFan(dockFolderPopupWidget_)
                    ? snowdesktop::collection_popup_layout::FanInsertionDistanceSquared(
                        snowdesktop::collection_popup_layout::FanInsertionLine(
                            GetCollectionPopupFanItem(popup, i), popupMetrics,
                            CollectionPopupFanRootAbove(), after), client)
                    : snowdesktop::popup_drag_rules::
                        InsertionEdgeDistanceSquared(
                            itemRect, clipped, client,
                            ((UsesCollectionPopupList(dockFolderPopupWidget_) && GetCollectionPopupColumnCount(popup) == 1) || UsesCollectionPopupFan(dockFolderPopupWidget_)),
                            after, gutter);
                if (distance >=
                    bestDistanceSquared)
                    continue;
                bestDistanceSquared =
                    distance;
                nearestIndex = i;
                nearestBounds = itemRect;
                nearestRegion =
                    edgeRegions[edge];
            }
        }
        if (bestDistanceSquared !=
            std::numeric_limits<
                long long>::max())
        {
            targetSlot = popupDragTarget_.BindPlacement(
                targetContainer,
                nearestBounds, nearestIndex,
                SlotFeedbackRole::Popup);
            targetRegion =
                nearestRegion;
        }
        return true;
    }

    if (popupWidgetIndex_ >= widgets_.size()) return false;

    RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
    if (!PtInRect(&popup, client)) return false;

    WidgetContainer* popupContainer = nullptr;
    for (auto& c : containers_)
    {
        popupContainer = dynamic_cast<WidgetContainer*>(c.get());
        if (popupContainer && popupContainer->GetWidgetData() == &widgets_[popupWidgetIndex_])
            break;
        popupContainer = nullptr;
    }
    if (!popupContainer) return false;

    std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
    RECT content = GetCollectionPopupContentRect(popup);
    targetContainer = popupContainer;
    targetSlot = nullptr;
    targetRegion = HitRegion::None;
    if (!CanCurrentDragUseCollectionPopup())
    {
        // The popup is the foreground surface, so consume the hit without
        // exposing a placement slot or passing through to the Dock below it.
        targetRegion = HitRegion::Blocked;
        return true;
    }
    if (client.y < content.top || client.y >= content.bottom)
        return true;

    size_t slotIndex = 0;
    RECT slotBounds = content;
    HitRegion region = HitRegion::Empty;

    if (popupKeys.empty())
    {
        RECT virtualItem =
            GetCollectionPopupItemRect(
                popup, 0);
        RECT visibleItem{};
        if (!IntersectRect(
                &visibleItem,
                &virtualItem,
                &content))
            visibleItem = content;
        targetSlot = popupDragTarget_.BindPlacement(
            popupContainer,
            visibleItem, 0, SlotFeedbackRole::Popup);
        targetRegion =
            HitRegion::SortBefore;
        return true;
    }

    for (size_t i = 0; i < popupKeys.size(); ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popup, i);
        RECT clipped{};
        if (!IntersectRect(&clipped, &itemRect, &content) ||
            (!HitTestCollectionPopupItem(popup, i, client) &&
             !HitTestCollectionPopupHandoff(popup, i, client)))
            continue;

        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
        if (snowdesktop::popup_drag_rules::
                CanHandoffToItem(
                    itemIndex != static_cast<size_t>(-1),
                    itemIndex != static_cast<size_t>(-1) &&
                        items_[itemIndex].selected))
        {
            if (HitTestCollectionPopupHandoff(popup, i, client))
            {
                region = HitRegion::Handoff;
                // Keep the icon-sized activation area, but present the same
                // full-cell handoff feedback used by desktop widgets.
                slotBounds =
                    snowdesktop::popup_drag_rules::
                        HandoffIndicatorBounds(
                            itemRect);
            }
        }

        slotIndex = i;
        if (region != HitRegion::Handoff)
            slotBounds = itemRect;
        if (region != HitRegion::Handoff)
        {
            region = snowdesktop::popup_drag_rules::
                    IsAfterInsertionMidpoint(
                        itemRect, client,
                        ((UsesCollectionPopupList(widgets_[popupWidgetIndex_]) && GetCollectionPopupColumnCount(popup) == 1) || UsesCollectionPopupFan(widgets_[popupWidgetIndex_])))
                ? HitRegion::SortAfter
                : HitRegion::SortBefore;
        }
        if (region != HitRegion::Handoff && UsesCollectionPopupFan(widgets_[popupWidgetIndex_]))
            region = snowdesktop::collection_popup_layout::FanIsAfterInsertion(
                GetCollectionPopupFanItem(popup, i), client, CollectionPopupFanRootAbove())
                ? HitRegion::SortAfter : HitRegion::SortBefore;
        if (region == HitRegion::Handoff)
        {
            targetSlot = popupDragTarget_.BindHandoff(
                popupContainer, slotBounds, slotIndex,
                SlotFeedbackRole::Popup,
                &items_[itemIndex],
                [&]() -> std::unique_ptr<Item> {
                    return std::make_unique<DesktopIcon>(
                        &items_[itemIndex],
                        popupContainer, this);
                });
        }
        else
        {
            targetSlot = popupDragTarget_.BindPlacement(
                popupContainer, slotBounds, slotIndex,
                SlotFeedbackRole::Popup);
        }
        targetRegion = region;
        return true;
    }

    long long bestDistanceSquared = std::numeric_limits<long long>::max();
    for (size_t i = 0; i < popupKeys.size(); ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popup, i);
        RECT clipped{};
        if (!IntersectRect(&clipped, &itemRect, &content))
            continue;

        const HitRegion edgeRegions[] = {
            HitRegion::SortBefore,
            HitRegion::SortAfter,
        };
        for (size_t edge = 0; edge < 2; ++edge)
        {
            const bool after = edge != 0;
            const auto popupMetrics =
                GetOpenCollectionPopupLayoutMetrics();
            const bool listMode =
                ((UsesCollectionPopupList(widgets_[popupWidgetIndex_]) && GetCollectionPopupColumnCount(popup) == 1) || UsesCollectionPopupFan(widgets_[popupWidgetIndex_]));
            const long gutter = UsesCollectionPopupList(widgets_[popupWidgetIndex_])
                ? static_cast<long>(snowdesktop::widget_item_layout::InsertionBoundaryPad(
                    GetCollectionPopupItemRect(popup, 0), GetCollectionPopupItemRect(popup, 1),
                    GetCollectionPopupColumnCount(popup) > 1))
                : listMode ? popupMetrics.gapY / 2 : popupMetrics.gapX / 2;
            const long long distanceSquared = UsesCollectionPopupFan(widgets_[popupWidgetIndex_])
                ? snowdesktop::collection_popup_layout::FanInsertionDistanceSquared(
                    snowdesktop::collection_popup_layout::FanInsertionLine(
                        GetCollectionPopupFanItem(popup, i), popupMetrics,
                        CollectionPopupFanRootAbove(), after), client)
                : snowdesktop::popup_drag_rules::
                    InsertionEdgeDistanceSquared(
                        itemRect, clipped, client,
                        listMode, after, gutter);
            if (distanceSquared >= bestDistanceSquared) continue;
            bestDistanceSquared = distanceSquared;
            slotIndex = i;
            slotBounds = itemRect;
            region = edgeRegions[edge];
        }
    }

    if (bestDistanceSquared == std::numeric_limits<long long>::max())
        return true;

    targetSlot = popupDragTarget_.BindPlacement(
        popupContainer,
        slotBounds, slotIndex, SlotFeedbackRole::Popup);
    targetRegion = region;
    return true;
}

/**
 * @brief 更新拖拽翻页热边的悬停和自动翻页状态
 * @param clientPoint 当前鼠标客户端坐标
 * @return 拖拽会话仍可继续时返回 true
 */
bool DesktopApp::UpdateDragPageNavigation(POINT clientPoint)
{
    lastMousePoint_ = clientPoint;
    if (desktopIconsHidden_)
    {
        SetPageNavHotEdgeHover(0);
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return dragSession_.IsActive();
    }
    if (!dragSession_.IsActive())
    {
        SetPageNavHotEdgeHover(0);
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return false;
    }

    RECT prevEdge{}, nextEdge{};
    GetNavHotEdgeRects(prevEdge, nextEdge);

    const auto target = snowdesktop::page_navigation_rules::
        HitTestPointerTarget(clientPoint, prevEdge, nextEdge);
    int navSide = snowdesktop::page_navigation_rules::
        PointerTargetDirection(target);
    const bool directionAvailable =
        (navSide == -1 && pageOffset_ > 0) ||
        (navSide == 1 && pageOffset_ < MaxPageOffset());
    if (!directionAvailable)
        navSide = 0;
    SetPageNavHotEdgeHover(navSide);

    // 自动翻页仅在可操作方向触发
    const bool navEnabled =
        (navSide == -1 && pageOffset_ > 0) ||
        (navSide == 1 && pageOffset_ < MaxPageOffset());
    if (navSide == 0 || !navEnabled)
    {
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return true;
    }

    const DWORD now = GetTickCount();
    if (navAutoFlipDir_ != navSide)
    {
        navAutoFlipDir_ = navSide;
        navAutoFlipTick_ = now;
        return true;
    }
    if (now - navAutoFlipTick_ <= 500)
        return true;

    const int newOffset = NextNonEmptyOffset(pageOffset_, navSide);
    navAutoFlipTick_ = now;
    if (newOffset == pageOffset_)
        return true;

    const bool hasInternalItems = !dragSession_.Items().empty();
    pageOffset_ = newOffset;
    ApplyPageMapping();
    // As with keyboard paging, only change the visible page. Moving selected
    // icons here can repack the group around occupied cells before release,
    // while its ghost still uses the drag-start snapshot.
    LayoutItems();
    RefreshPageNavHotEdgeHoverAt(clientPoint);
    if (!dragSession_.IsActive() || (hasInternalItems && dragSession_.Items().empty()))
    {
        mouseDownHit_ = nullptr;
        mouseDown_ = false;
        return false;
    }

    InvalidateDragStaticScene();
    // 页面迁移后 usedSlots 变化，预览缓存失效
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
    return true;
}

/**
 * @brief 清除所有桌面项、小部件和文件夹条目的选中状态
 */
