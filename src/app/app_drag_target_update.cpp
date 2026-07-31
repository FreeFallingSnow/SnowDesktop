#include "app.h"

// Drag-target resolution, popup hit testing and page-navigation dwell.

void DesktopApp::RefreshDragTargetAt(POINT clientPoint, int mods)
{
    if (!dragSession_.IsActive()) return;

    dragSession_.UpdatePoint(clientPoint);

    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    popupDragTargetSlot_.reset();

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
        HitTestPopupForDrag(clientPoint, targetContainer, targetSlot, targetRegion);

    if (!popupHit && !targetContainer)
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

    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion,
            dragSession_.Items(), dragSession_.Source(), mods);
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
            popupDragTargetSlot_ =
                std::make_unique<Slot>(
                    targetContainer,
                    visibleItem, 0);
            targetSlot =
                popupDragTargetSlot_.get();
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
            RECT handoffRect =
                GetItemIconRect(itemRect);
            InflateRect(&handoffRect, -4, -4);
            if (entry.isDirectory &&
                PtInRect(&handoffRect, client))
            {
                RECT handoffVisual =
                    GetItemIconRect(itemRect);
                InflateRect(
                    &handoffVisual, 4, 4);
                RECT clippedVisual{};
                if (IntersectRect(
                        &clippedVisual,
                        &handoffVisual,
                        &content))
                    handoffVisual =
                        clippedVisual;
                popupDragTargetSlot_ =
                    std::make_unique<Slot>(
                        targetContainer,
                        handoffVisual, i);
                popupDragTargetSlot_->SetItem(
                    dockFolderPopupContainer_->
                        GetMemberItem(i));
                targetSlot =
                    popupDragTargetSlot_.get();
                targetRegion = HitRegion::Handoff;
                return true;
            }

            popupDragTargetSlot_ =
                std::make_unique<Slot>(
                    targetContainer,
                    itemRect, i);
            targetSlot =
                popupDragTargetSlot_.get();
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
            popupDragTargetSlot_ =
                std::make_unique<Slot>(
                    targetContainer,
                    nearestBounds,
                    nearestIndex);
            targetSlot =
                popupDragTargetSlot_.get();
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
    if (!PtInRect(&content, client))
        return true;

    size_t slotIndex = 0;
    RECT slotBounds = content;
    HitRegion region = HitRegion::Empty;
    Item* handoffItem = nullptr;

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
        popupDragTargetSlot_ =
            std::make_unique<Slot>(
                popupContainer,
                visibleItem, 0);
        targetSlot = popupDragTargetSlot_.get();
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
        if (itemIndex != static_cast<size_t>(-1) && !items_[itemIndex].selected)
        {
            RECT iconRect = GetItemIconRect(itemRect);
            RECT handoffRect = { iconRect.left - 4, iconRect.top - 2,
                                 iconRect.right + 4, iconRect.bottom + 4 };
            if (PtInRect(&handoffRect, client))
            {
                region = HitRegion::Handoff;
                handoffItem = popupContainer->GetMemberItem(i);
                RECT clippedHandoff{};
                if (IntersectRect(
                        &clippedHandoff,
                        &handoffRect,
                        &content))
                    slotBounds =
                        clippedHandoff;
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
        popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
        if (handoffItem)
            popupDragTargetSlot_->SetItem(handoffItem);
        targetSlot = popupDragTargetSlot_.get();
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

    popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
    targetSlot = popupDragTargetSlot_.get();
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
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return dragSession_.IsActive();
    }
    if (!dragSession_.IsActive())
    {
        navHoverSide_ = 0;
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return false;
    }

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    int navSide = 0;
    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();
    // 悬停检测不限制 hasPrev/hasNext，让置灰按钮也有 hover 视觉反馈
    if (PtInRect(&prevRect, clientPoint)) navSide = -1;
    else if (PtInRect(&nextRect, clientPoint)) navSide = 1;
    navHoverSide_ = navSide;

    // 自动翻页仅在可操作方向触发
    const bool navEnabled = (navSide == -1 && hasPrev) || (navSide == 1 && hasNext);
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
    if (newOffset == pageOffset_)
        return true;

    const bool hasInternalItems = !dragSession_.Items().empty();
    const bool groupedEntryDrag =
        dragSession_.SourceList().
            hasCollectionGroupEntries ||
        dragSession_.SourceList().
            hasFileGroupEntries;
    // 保存迁移前第一个选中项的实际 bounds（含页面渲染尺寸差异）
    RECT oldFirstBounds{};
    bool hasOldBounds = false;
    if (hasInternalItems && !dragSession_.Items().empty())
    {
        for (const auto& item : items_)
        {
            if (item.selected && !item.name.empty())
            {
                oldFirstBounds = item.bounds;
                hasOldBounds = !IsRectEmptyRect(oldFirstBounds);
                break;
            }
        }
    }
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (hasInternalItems && !groupedEntryDrag)
        MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();

    navAutoFlipTick_ = now;
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
        // 用实际 bounds 差值补偿 mouseDown，消除跨页渲染尺寸差异导致的视觉跳动
        if (hasOldBounds)
        {
            for (const auto& item : items_)
            {
                if (item.selected && !item.name.empty())
                {
                    dragSession_.AdjustMouseDownPoint({
                        item.bounds.left - oldFirstBounds.left,
                        item.bounds.top  - oldFirstBounds.top
                    });
                    break;
                }
            }
        }
        else
        {
            UpdateDragGroupOrigin();
        }
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
