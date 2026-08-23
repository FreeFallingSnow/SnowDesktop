#include "app.h"
#include "../page_navigation_rules.h"
#include "../core/drag_source_rebind.h"
#include "../core/transient_drag_slot.h"

// Drag-target resolution, popup hit testing and page-navigation dwell.

void DesktopApp::ResolveCurrentDragTargetAt(POINT clientPoint)
{
    if (!dragSession_.IsActive()) return;

    dragSession_.UpdatePoint(clientPoint);

    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    if (dragDropController_.IsExternalDragActive())
    {
        if (!HitTestPopupForDrag(
                clientPoint, targetContainer,
                targetSlot, targetRegion))
        {
            const DragTargetResolution resolved =
                dragDropController_.ResolveExternalTarget(
                    containers_, clientPoint,
                    [&](const Container& candidate) {
                        return !desktopIconsHidden_ ||
                            IsRetainedContainer(&candidate);
                    });
            targetContainer = resolved.container;
            targetSlot = resolved.slot;
            targetRegion = resolved.region;
        }
    }
    else
    {
        const bool suppressDesktopWidgetTargets =
            SuppressDesktopWidgetDragTargets();
        const bool groupedEntryDrag =
            dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries;
        const bool popupHit =
            !suppressDesktopWidgetTargets &&
            !groupedEntryDrag &&
            HitTestPopupForDrag(
                clientPoint, targetContainer,
                targetSlot, targetRegion);
        if (!popupHit)
        {
            const DragTargetResolution resolved =
                dragDropController_.ResolveInternalTarget(
                    containers_, clientPoint,
                    [&](const Container& candidate) {
                        if (desktopIconsHidden_ &&
                            !IsRetainedContainer(&candidate))
                            return false;
                        return !suppressDesktopWidgetTargets ||
                            (!dynamic_cast<const DesktopGrid*>(&candidate) &&
                             !dynamic_cast<const WidgetContainer*>(&candidate));
                    });
            targetContainer = resolved.container;
            targetSlot = resolved.slot;
            targetRegion = resolved.region;
        }
    }

    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);
}

void DesktopApp::RefreshDragTargetAt(POINT clientPoint, int mods)
{
    if (!dragSession_.IsActive()) return;

    ResolveCurrentDragTargetAt(clientPoint);

    std::wstring hint;
    if (dragSession_.TargetContainer() &&
        dragSession_.TargetRegion() != HitRegion::None)
    {
        hint = dragSession_.TargetContainer()->GetDragHint(
            dragSession_.TargetSlot(), dragSession_.TargetRegion(),
            dragSession_.Items(), dragSession_.Source(), mods);
    }
    ShowDragHintWindow(clientPoint, hint);
    InvalidateFloatingDockWindow(true);
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
    if (reboundItems.empty())
    {
        EndDragSession();
        return;
    }
    DragSourceList reboundList = BuildDragSourceList(reboundItems, source);
    dragSession_.RebindSource(source, std::move(reboundItems), std::move(reboundList));
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
    if (!IsCollectionPopupInteractive())
        return false;
    if (desktopIconsHidden_ && !IsOpenPopupRetained())
        return false;

    if (dockFolderPopupOpen_ &&
        dockFolderPopupContainer_)
    {
        const RECT popup = GetCollectionPopupRect(
            dockFolderPopupWidget_);
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
        if (!PtInRect(&content, client))
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
             i < dockFolderPopupWidget_.
                folderEntries.size(); ++i)
        {
            RECT itemRect =
                GetCollectionPopupItemRect(popup, i);
            RECT clipped{};
            if (!IntersectRect(
                    &clipped, &itemRect, &content) ||
                !PtInRect(&clipped, client))
                continue;

            FolderEntry& entry =
                dockFolderPopupWidget_.folderEntries[i];
            const RECT handoffRect =
                snowdesktop::popup_drag_rules::
                    HandoffActivationBounds(
                        GetItemIconRect(itemRect));
            if (snowdesktop::popup_drag_rules::
                    CanHandoffToItem(
                        true, entry.selected) &&
                PtInRect(&handoffRect, client))
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
                client.x <
                    itemRect.left +
                        (itemRect.right -
                         itemRect.left) / 2
                ? HitRegion::SortBefore
                : HitRegion::SortAfter;
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
            i < dockFolderPopupWidget_.
                folderEntries.size(); ++i)
        {
            const RECT itemRect =
                GetCollectionPopupItemRect(
                    popup, i);
            RECT clipped{};
            if (!IntersectRect(
                    &clipped, &itemRect,
                    &content))
                continue;
            const LONG edgeXs[] = {
                itemRect.left -
                    kCollectionPopupGapX / 2,
                itemRect.right +
                    kCollectionPopupGapX / 2,
            };
            const HitRegion edgeRegions[] = {
                HitRegion::SortBefore,
                HitRegion::SortAfter,
            };
            for (size_t edge = 0;
                edge < 2; ++edge)
            {
                const long long dx =
                    static_cast<long long>(
                        client.x) -
                    edgeXs[edge];
                long long dy = 0;
                if (client.y < clipped.top)
                    dy =
                        static_cast<long long>(
                            clipped.top) -
                        client.y;
                else if (client.y >=
                    clipped.bottom)
                    dy =
                        static_cast<long long>(
                            client.y) -
                        clipped.bottom + 1;
                const long long distance =
                    dx * dx + dy * dy;
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
    if (!PtInRect(&content, client))
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
            !PtInRect(&clipped, client))
            continue;

        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
        if (snowdesktop::popup_drag_rules::
                CanHandoffToItem(
                    itemIndex != static_cast<size_t>(-1),
                    itemIndex != static_cast<size_t>(-1) &&
                        items_[itemIndex].selected))
        {
            RECT iconRect = GetItemIconRect(itemRect);
            const RECT handoffRect =
                snowdesktop::popup_drag_rules::
                    HandoffActivationBounds(iconRect);
            if (PtInRect(&handoffRect, client))
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
            region = client.x < itemRect.left + (itemRect.right - itemRect.left) / 2
                ? HitRegion::SortBefore : HitRegion::SortAfter;
        }
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

        const LONG edgeXs[] = {
            itemRect.left - kCollectionPopupGapX / 2,
            itemRect.right + kCollectionPopupGapX / 2,
        };
        const HitRegion edgeRegions[] = {
            HitRegion::SortBefore,
            HitRegion::SortAfter,
        };
        for (size_t edge = 0; edge < 2; ++edge)
        {
            const long long dx = static_cast<long long>(client.x) - edgeXs[edge];
            long long dy = 0;
            if (client.y < clipped.top)
                dy = static_cast<long long>(clipped.top) - client.y;
            else if (client.y >= clipped.bottom)
                dy = static_cast<long long>(client.y) - clipped.bottom + 1;
            const long long distanceSquared = dx * dx + dy * dy;
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
 * @brief 更新拖拽翻页按钮的悬停和自动翻页状态
 * @param clientPoint 当前鼠标客户端坐标
 * @return 拖拽会话仍可继续时返回 true
 */
bool DesktopApp::UpdateDragPageNavigation(POINT clientPoint)
{
    lastMousePoint_ = clientPoint;
    if (desktopIconsHidden_)
    {
        navHoverSide_ = 0;
        navHotEdgeHover_ = false;
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return dragSession_.IsActive();
    }
    if (!dragSession_.IsActive())
    {
        navHoverSide_ = 0;
        navHotEdgeHover_ = false;
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return false;
    }

    RECT prevRect{}, nextRect{};
    RECT prevEdge{}, nextEdge{};
    GetNavButtonRects(prevRect, nextRect);
    GetNavHotEdgeRects(prevEdge, nextEdge);

    const auto target = snowdesktop::page_navigation_rules::
        HitTestPointerTarget(
            clientPoint, prevRect, nextRect,
            prevEdge, nextEdge);
    int navSide = snowdesktop::page_navigation_rules::
        PointerTargetDirection(target);
    const bool navHotEdge = snowdesktop::page_navigation_rules::
        IsEdgeTarget(target);
    // 悬停检测不限制 hasPrev/hasNext，让置灰按钮也有 hover 视觉反馈
    const bool directionAvailable =
        (navSide == -1 && pageOffset_ > 0) ||
        (navSide == 1 && pageOffset_ < MaxPageOffset());
    if (navHotEdge && !directionAvailable)
        navSide = 0;
    navHoverSide_ = navSide;
    navHotEdgeHover_ = navSide != 0 && navHotEdge;

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
    const bool groupedEntryDrag =
        dragSession_.SourceList().
            hasCollectionGroupEntries ||
        dragSession_.SourceList().
            hasFileGroupEntries;
    const POINT oldGroupOrigin{
        dragGroupOriginX_, dragGroupOriginY_ };
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (hasInternalItems && !groupedEntryDrag)
        MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();
    if (!dragSession_.IsActive() || (hasInternalItems && dragSession_.Items().empty()))
    {
        mouseDownHit_ = nullptr;
        mouseDown_ = false;
        return false;
    }

    InvalidateDragStaticScene();
    if (hasInternalItems && !groupedEntryDrag)
    {
        UpdateDragGroupOrigin();
        dragSession_.AdjustForGroupOriginChange(
            oldGroupOrigin,
            { dragGroupOriginX_, dragGroupOriginY_ });
    }
    // 页面迁移后 usedSlots 变化，预览缓存失效
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
    SaveLayoutSlots();
    return true;
}

/**
 * @brief 清除所有桌面项、小部件和文件夹条目的选中状态
 */
