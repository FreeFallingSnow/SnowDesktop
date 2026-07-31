#include "app.h"

// Page-navigation clicks and right-button context dispatch.

bool DesktopApp::HandlePageNavClick(POINT point)
{
    if (gridPages_.empty()) return false;
    if (MaxPageOffset() <= 0) return false;   // 无溢出页时不处理

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    int delta = 0;
    if (PtInRect(&prevRect, point)) delta = -1;
    else if (PtInRect(&nextRect, point)) delta = 1;

    // 点击落在导航按钮区域内但方向不可用 → 拦截点击（不穿透到下方图标）
    if (delta != 0 && !((delta == -1 && hasPrev) || (delta == 1 && hasNext)))
        return true;
    if (delta == 0) return false;

    int newOffset = NextNonEmptyOffset(pageOffset_, delta);
    if (newOffset == pageOffset_) return false;

    bool wasDragging = dragSession_.IsActive();
    // 保存迁移前第一个选中项的实际 bounds（含页面渲染尺寸差异）
    RECT oldFirstBounds{};
    bool hasOldBounds = false;
    if (wasDragging)
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
    if (wasDragging) MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();
    if (wasDragging && !dragSession_.IsActive())
    {
        mouseDownHit_ = nullptr;
        mouseDown_ = false;
    }
    wasDragging = wasDragging && dragSession_.IsActive();
    if (wasDragging) InvalidateDragStaticScene();
    if (wasDragging)
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
    }
    // 页面迁移后预览缓存失效
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

/**
 * @brief 处理鼠标右键释放事件（显示上下文菜单）
 * @param lp LPARAM（含鼠标坐标）
 */
void DesktopApp::OnRightButtonUp(LPARAM lp)
{
    if (renameEdit_ != nullptr) return;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT screenPt = pt;
    ClientToScreen(hwnd_, &screenPt);

    if (DockContainer* dock = GetDockContainerAtPoint(pt))
    {
        if (DockEntryItem* dockItem = dock->EntryAtPoint(pt))
        {
            const size_t entryIndex = dockItem->GetEntryIndex();
            if (entryIndex < dockEntries_.size())
            {
                ClearSelection();
                dockItem->SetSelected(true);
                const RECT dockItemBounds = dock->GetElementVisualRect(
                    dockItem->GetBounds(), pt);
                if (dockItem->GetEntryType() == DockEntryType::DesktopItem)
                {
                    size_t itemIndex = FindItemIndexByKey(dockItem->GetReference());
                    if (itemIndex < items_.size())
                    {
                        items_[itemIndex].selected =
                            dockItem->IsSelected();
                        items_[itemIndex].bounds = dockItemBounds;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        if (IsProtectedDesktopIcon(items_[itemIndex]))
                            ShowShellContextMenu(
                                screenPt,
                                static_cast<int>(itemIndex),
                                false, dockItemBounds);
                        else
                            ShowItemContextMenu(
                                screenPt,
                                static_cast<int>(itemIndex),
                                false, false, dockItemBounds,
                                dockEntries_[entryIndex].
                                        keepOnDesktop
                                    ? std::optional<size_t>(
                                          entryIndex)
                                    : std::nullopt,
                                true);
                    }
                }
                else
                {
                    size_t widgetIndex = FindWidgetIndexById(dockItem->GetReference());
                    if (widgetIndex < widgets_.size())
                    {
                        widgets_[widgetIndex].selected =
                            dockItem->IsSelected();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        ShowWidgetContextMenu(
                            screenPt, widgetIndex,
                            dockItemBounds);
                    }
                }
                return;
            }
        }

        if (DockFrequentItem* frequentItem = dock->FrequentItemAtPoint(pt))
        {
            const size_t itemIndex = frequentItem->GetItemIndex();
            if (itemIndex < items_.size())
            {
                ClearSelection();
                frequentItem->SetSelected(true);
                items_[itemIndex].bounds = dock->GetElementVisualRect(
                    frequentItem->GetBounds(), pt);
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (IsProtectedDesktopIcon(items_[itemIndex]))
                    ShowShellContextMenu(
                        screenPt,
                        static_cast<int>(itemIndex),
                        false, dock->GetElementVisualRect(
                            frequentItem->GetBounds(), pt));
                else
                    ShowItemContextMenu(
                        screenPt,
                        static_cast<int>(itemIndex),
                        true, false,
                        dock->GetElementVisualRect(
                            frequentItem->GetBounds(), pt),
                        std::nullopt, true);
                return;
            }
        }

        if (DockRunningItem* runningItem =
                dock->RunningItemAtPoint(pt))
        {
            const size_t runningIndex =
                runningItem->GetRunningIndex();
            if (runningIndex <
                dockUnpinnedRunningApps_.size())
            {
                ClearSelection();
                DismissDockWindowPreviewUntilLeave();
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                ShowDockRunningAppContextMenu(
                    screenPt, runningIndex);
                return;
            }
        }

        if (dock->ContainsInteractivePoint(pt))
        {
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
            ShowDockContextMenu(screenPt);
            return;
        }
    }

    if (IsCollectionPopupInteractive() &&
        dockFolderPopupOpen_)
    {
        RECT popup = GetCollectionPopupRect(
            dockFolderPopupWidget_);
        if (PtInRect(&popup, pt))
        {
            RECT content =
                GetCollectionPopupContentRect(popup);
            for (size_t i = 0;
                 i < dockFolderPopupWidget_.
                    folderEntries.size(); ++i)
            {
                RECT itemRect =
                    GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top =
                    std::max(clipped.top, content.top);
                clipped.bottom =
                    std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top ||
                    !PtInRect(&clipped, pt))
                    continue;
                ClearSelection();
                if (!dockFolderPopupWidget_.
                        folderEntries[i].selected)
                {
                    for (auto& entry :
                         dockFolderPopupWidget_.
                            folderEntries)
                        entry.selected = false;
                    dockFolderPopupWidget_.
                        folderEntries[i].
                            selected = true;
                }
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                ShowDockFolderPopupContextMenu(
                    screenPt, i);
                return;
            }
            ShowDockFolderPopupContextMenu(
                screenPt);
            return;
        }
    }
    else if (IsCollectionPopupInteractive() &&
        popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt))
        {
            std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
            RECT content = GetCollectionPopupContentRect(popup);
            for (size_t i = 0; i < popupKeys.size(); ++i)
            {
                RECT itemRect = GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top = std::max(clipped.top, content.top);
                clipped.bottom = std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;

                size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                if (itemIndex != static_cast<size_t>(-1))
                {
                    if (!items_[itemIndex].selected)
                        SelectOnly(static_cast<int>(itemIndex));
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (IsProtectedDesktopIcon(items_[itemIndex]))
                        ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
                    else
                        ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
                    return;
                }
            }
        }
    }

    // File-group source tabs own their context menu; do not let the
    // surrounding widget frame consume a tab right-click.
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(it->get()))
            continue;
        auto* group =
            dynamic_cast<FileGroup*>(it->get());
        if (!group) continue;
        const std::wstring childId =
            group->SourceIdAtPoint(pt);
        if (childId.empty()) continue;
        DesktopWidget* groupData = group->GetWidgetData();
        const size_t groupIndex = groupData
            ? FindWidgetIndexById(groupData->id)
            : static_cast<size_t>(-1);
        const size_t childIndex =
            FindWidgetIndexById(childId);
        if (groupIndex >= widgets_.size() ||
            childIndex >= widgets_.size())
            break;

        const auto childIds =
            group->GetVisibleSourceIds();
        const auto childIt = std::find(
            childIds.begin(), childIds.end(), childId);
        const size_t tabIndex =
            childIt == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), childIt));
        ClearSelection();
        widgets_[groupIndex].activeCategoryId = childId;
        widgets_[groupIndex].scrollOffset = 0;
        widgets_[childIndex].selected = true;
        group->InvalidateHostedView();
        group->EnsureSourceTabVisible(tabIndex);
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowFileGroupSourceTabContextMenu(
            screenPt, groupIndex, childId);
        return;
    }

    // Collection-group tabs own their context menu.
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(it->get()))
            continue;
        auto* group =
            dynamic_cast<CollectionGroup*>(it->get());
        if (!group) continue;
        const std::wstring collectionId =
            group->CategoryIdAtPoint(pt);
        if (collectionId.empty()) continue;

        DesktopWidget* groupData =
            group->GetWidgetData();
        size_t groupIndex =
            static_cast<size_t>(-1);
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (&widgets_[i] == groupData)
            {
                groupIndex = i;
                break;
            }
        }
        const size_t childIndex =
            FindWidgetIndexById(collectionId);
        if (groupIndex >= widgets_.size() ||
            childIndex >= widgets_.size())
            break;

        const auto& childIds =
            group->GetVisibleCollectionIds();
        auto childIt = std::find(
            childIds.begin(), childIds.end(),
            collectionId);
        const size_t tabIndex =
            childIt == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), childIt));
        ClearSelection();
        widgets_[groupIndex].activeCategoryId =
            collectionId;
        widgets_[groupIndex].scrollOffset = 0;
        widgets_[childIndex].selected = true;
        group->InvalidateFilterCache();
        group->EnsureTabVisible(tabIndex);
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowCollectionGroupTabContextMenu(
            screenPt, groupIndex, collectionId);
        return;
    }

    // Check widget member items first; otherwise the widget frame menu steals member right-clicks.
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(it->get()))
            continue;
        auto* wc = dynamic_cast<WidgetContainer*>(it->get());
        if (!wc) continue;

        WidgetHit wh = wc->HitTestWidget(pt);
        if (wh == WidgetHit::MoveHandle || wh == WidgetHit::ResizeHandle)
            continue;

        RECT bodyRect = wc->GetBodyRect();

        auto& slots = wc->GetSlots();
        for (auto& slot : slots)
        {
            if (!slot) continue;
            RECT bounds = slot->GetBounds();
            if (!PtInRect(&bounds, pt)) continue;
            if (!PtInRect(&bodyRect, pt)) continue;

            auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem());
            DesktopItem* item = icon ? icon->GetDesktopItem() : nullptr;
            if (!item)
            {
                auto* folderIcon = dynamic_cast<FolderEntryIcon*>(slot->GetItem());
                FolderEntry* entry = folderIcon ? folderIcon->GetFolderEntry() : nullptr;
                if (!entry) break;

                auto* folderWidget = dynamic_cast<WidgetContainer*>(wc);
                DesktopWidget* data = folderWidget ? folderWidget->GetWidgetData() : nullptr;
                size_t widgetIndex = static_cast<size_t>(-1);
                size_t memberIndex = static_cast<size_t>(-1);
                for (size_t wi = 0; wi < widgets_.size(); ++wi)
                {
                    if (&widgets_[wi] != data) continue;
                    widgetIndex = wi;
                    for (size_t mi = 0; mi < widgets_[wi].folderEntries.size(); ++mi)
                    {
                        if (&widgets_[wi].folderEntries[mi] == entry)
                        {
                            memberIndex = mi;
                            break;
                        }
                    }
                    break;
                }
                if (widgetIndex == static_cast<size_t>(-1) ||
                    memberIndex == static_cast<size_t>(-1))
                    break;

                if (!entry->selected)
                {
                    ClearSelection();
                    widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                ShowFolderEntryContextMenu(screenPt, widgetIndex, memberIndex);
                return;
            }

            size_t itemIndex = FindItemIndexByKey(item->layoutKey);
            if (itemIndex == static_cast<size_t>(-1)) break;

            if (!items_[itemIndex].selected)
                SelectOnly(static_cast<int>(itemIndex));
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (IsProtectedDesktopIcon(items_[itemIndex]))
                ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
            else
                ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
            return;
        }
    }

    // Check widget hit after member items.
    size_t hitWidget = static_cast<size_t>(-1);
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        if (desktopIconsHidden_ &&
            !widgets_[wi].keepWhenDesktopHidden)
            continue;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgets_[wi]) continue;
            if (wc->HitTestWidget(pt) != WidgetHit::None)
            {
                hitWidget = wi;
                break;
            }
        }
        if (hitWidget != static_cast<size_t>(-1)) break;
    }

    if (hitWidget != static_cast<size_t>(-1))
    {
        // Select the widget and show its context menu
        SelectWidgetOnly(hitWidget);
        InvalidateRect(hwnd_, nullptr, FALSE);

        ShowWidgetContextMenu(screenPt, hitWidget);
        return;
    }

    size_t hitStandaloneWidget = HitTestStandaloneWidgetIndex(pt);
    if (hitStandaloneWidget != static_cast<size_t>(-1))
    {
        SelectWidgetOnly(hitStandaloneWidget);
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowWidgetContextMenu(screenPt, hitStandaloneWidget);
        return;
    }

    if (IsPointOverWidgetChrome(pt))
    {
        ClearSelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowBackgroundContextMenu(screenPt);
        return;
    }

    int hit = HitTestItem(pt);
    if (hit >= 0 && !items_[hit].selected)
        SelectOnly(hit);
    else if (hit < 0)
        ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (hit >= 0)
    {
        if (IsProtectedDesktopIcon(items_[hit]))
            ShowShellContextMenu(screenPt, hit);
        else
            ShowItemContextMenu(screenPt, hit);
    }
    else
        ShowBackgroundContextMenu(screenPt);
}

/**
 * @brief 处理定时器事件
 * @param timerId 定时器 ID
 */
