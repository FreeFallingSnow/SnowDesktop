#include "app.h"

// Pointer leave, Dock click release and primary-button drag completion.

void DesktopApp::OnMouseLeave()
{
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const bool pointerStillInteractsWithDockPreview =
        dockWindowPreview_ &&
        dockWindowPreview_->ContainsInteractionPoint(cursorScreen);
    if (pointerStillInteractsWithDockPreview)
        dockWindowPreview_->ScheduleHide();
    else
        HideDockWindowPreview();
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;

    popupDwellController_.Reset();
    collectionGroupTabDwellWidgetIndex_ =
        static_cast<size_t>(-1);
    collectionGroupTabDwellId_.clear();
    collectionGroupTabDwellTick_ = 0;
    dockHandoffDwellIndex_ = static_cast<size_t>(-1);
    dockHandoffDwellStartTick_ = 0;
    dockHandoffDwellReady_ = false;
    if (hwnd_)
    {
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        KillTimer(hwnd_, kCollectionGroupTabDwellTimerId);
        KillTimer(hwnd_, kDockHandoffDwellTimerId);
    }

    // Capture-based dragging continues to receive coordinates outside the
    // window. Preserve that pointer state, but clear passive hover immediately.
    // The preview owns its independent screen-space transition triangle, so
    // Dock magnification can still be reset while the preview stays reachable.
    const HWND captureWindow = GetCapture();
    const bool ownsInteractionCapture =
        captureWindow == hwnd_ ||
        captureWindow == floatingDockHwnd_;
    if (!ownsInteractionCapture && !mouseDown_ &&
        !dragSession_.IsActive() &&
        widgetAction_ == WidgetAction::None)
    {
        lastMousePoint_ = { LONG_MIN, LONG_MIN };
    }
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

bool DesktopApp::HandleDockClickRelease(POINT point)
{
    DockContainer* dock = dockPressedContainer_;
    if (!dock) dock = GetDockContainerAtPoint(point);
    if (!dock) return false;

    DockEntryType entryType = DockEntryType::DesktopItem;
    std::wstring reference;
    size_t frequentItemIndex = static_cast<size_t>(-1);
    std::wstring runningAppKey;
    const size_t pressedEntryIndex = dockPressedEntry_;
    const size_t pressedFrequentItemIndex = dockPressedFrequentItem_;
    const auto pressedWindowAction = dockPressedWindowAction_;
    const HWND pressedTargetWindow = dockPressedTargetWindow_;
    std::optional<RECT> pressedAnchorScreen;
    if (mouseDownHit_ && hwnd_)
    {
        RECT anchor = dock->GetElementVisualRect(
            mouseDownHit_->GetBounds(), mouseDownPoint_);
        if (!IsRectEmpty(&anchor))
        {
            MapWindowPoints(
                hwnd_, nullptr,
                reinterpret_cast<POINT*>(&anchor), 2);
            pressedAnchorScreen = anchor;
        }
    }
    if (dockPressedEntry_ < dockEntries_.size())
    {
        DockEntryItem* hit = dock->EntryAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetEntryIndex() != dockPressedEntry_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        entryType = dockEntries_[dockPressedEntry_].type;
        reference = dockEntries_[dockPressedEntry_].reference;
    }
    else if (!dockPressedRunningAppKey_.empty())
    {
        DockRunningItem* hit = dock->RunningItemAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetIdentityKey() != dockPressedRunningAppKey_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        runningAppKey = dockPressedRunningAppKey_;
    }
    else if (dockPressedFrequentItem_ < items_.size())
    {
        DockFrequentItem* hit = dock->FrequentItemAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetItemIndex() != dockPressedFrequentItem_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        frequentItemIndex = dockPressedFrequentItem_;
    }
    else
    {
        return false;
    }
    const bool folderEntry =
        pressedEntryIndex < dockEntries_.size() &&
        IsFolderDockEntry(dockEntries_[pressedEntryIndex]);

    size_t appItemIndex = frequentItemIndex;
    if (appItemIndex >= items_.size() && entryType == DockEntryType::DesktopItem)
        appItemIndex = FindItemIndexByKey(reference);
    bool waitForDoubleClick = false;
    if (appItemIndex < items_.size() && !folderEntry)
        waitForDoubleClick =
            pressedWindowAction ==
                snowdesktop::dock_window_rules::DockClickAction::Launch;

    // 在激活外部应用前完整结束本次桌面交互，避免鼠标捕获、选择高亮或
    // 拖拽预览跨到新前台窗口后仍残留。
    EndDragSession();
    HideDragHintWindow();
    if (!waitForDoubleClick)
        ClearSelection();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    dockPressedContainer_ = nullptr;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    ReleaseCapture();
    InvalidateDragStaticScene();
    const bool popupEntryOnFloatingDock =
        (entryType == DockEntryType::Collection ||
         folderEntry) &&
        floatingDockVisible_ &&
        dock == floatingDockContainer_;
    if (hwnd_ && !popupEntryOnFloatingDock)
    {
        if (floatingDockVisible_ &&
            dock == floatingDockContainer_)
            InvalidateFloatingDockWindow(true);
        else
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
    }

    if (waitForDoubleClick)
    {
        dockPendingDoubleClickEntry_ = pressedEntryIndex;
        dockPendingDoubleClickFrequentItem_ = pressedFrequentItemIndex;
        dockPendingDoubleClickTick_ = GetTickCount();
        return true;
    }

    dockPendingDoubleClickEntry_ = static_cast<size_t>(-1);
    dockPendingDoubleClickFrequentItem_ = static_cast<size_t>(-1);
    dockPendingDoubleClickTick_ = 0;

    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockClickRelease(
                pressedEntryIndex,
                dockSuppressClickReleaseEntry_))
    {
        dockSuppressClickReleaseEntry_ =
            static_cast<size_t>(-1);
        return true;
    }

    const auto prepareFloatingDockForWindowCommand =
        [&]() {
            if (!snowdesktop::dock_window_rules::
                    MustCloseFloatingDockBeforeWindowCommand(
                        floatingDockVisible_,
                        pressedWindowAction))
                return;
            CloseFloatingDock();
            // ShowWindow(SW_HIDE) 与 DComp 提交不在同一个时序域。等待
            // 顶层 Dock 真正退出合成场景后再抓屏，避免快照残留 Dock。
            DwmFlush();
        };

    if (!runningAppKey.empty())
    {
        prepareFloatingDockForWindowCommand();
        const auto running = std::find_if(dockUnpinnedRunningApps_.begin(),
            dockUnpinnedRunningApps_.end(), [&](const DockRunningAppInfo& app) {
                return app.identityKey == runningAppKey;
            });
        if (running != dockUnpinnedRunningApps_.end())
        {
            ActivateOrToggleDockWindow(
                running->window, pressedWindowAction,
                pressedTargetWindow,
                pressedAnchorScreen);
        }
    }
    else if (frequentItemIndex < items_.size())
    {
        prepareFloatingDockForWindowCommand();
        ActivateOrToggleDockItem(
            frequentItemIndex, pressedWindowAction,
            pressedTargetWindow,
            pressedAnchorScreen);
    }
    else if (entryType == DockEntryType::Collection)
    {
        const size_t widgetIndex = FindWidgetIndexById(reference);
        if (widgetIndex < widgets_.size())
        {
            if (IsCollectionPopupInteractive() &&
                snowdesktop::floating_dock_rules::
                    ShouldCloseCollectionPopup(
                        popupWidgetIndex_,
                        widgetIndex))
                CloseCollectionPopup();
            else
                OpenCollectionPopupAt(
                    widgetIndex, point);
        }
    }
    else if (folderEntry)
    {
        const std::wstring sourceId =
            std::to_wstring(static_cast<int>(entryType)) +
            L":" + ToUpperInvariant(reference);
        if (IsCollectionPopupInteractive() &&
            dockFolderPopupOpen_ &&
            dockFolderPopupSourceId_ == sourceId)
            CloseCollectionPopup();
        else
            OpenDockFolderPopupAt(
                pressedEntryIndex, point);
        dockPendingDoubleClickEntry_ =
            pressedEntryIndex;
        dockPendingDoubleClickTick_ =
            GetTickCount();
    }
    else
    {
        prepareFloatingDockForWindowCommand();
        const size_t itemIndex = FindItemIndexByKey(reference);
        if (itemIndex < items_.size())
        {
            ActivateOrToggleDockItem(
                itemIndex, pressedWindowAction,
                pressedTargetWindow,
                pressedAnchorScreen);
        }
    }
    return true;
}

void DesktopApp::OnLeftButtonUp(WPARAM wp, LPARAM lp)
{
    if (middleButtonWidgetMove_) return;
    (void)wp;
    POINT upPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int dropPreviewMods = 0;
    bool commitVisualBeforeDrop = false;
    HideDragHintWindow();

    if (luaWidgetPanelMouseDown_ &&
        !luaWidgetPanelRequest_.widgetId.empty())
    {
        const std::wstring panelWidgetId =
            luaWidgetPanelRequest_.widgetId;
        const RECT content =
            GetLuaWidgetPanelContentRect();
        const int localX =
            upPoint.x - content.left;
        const int localY =
            upPoint.y - content.top;
        const bool hostInputHandled =
            widgetEngine_ &&
            widgetEngine_->HandleHostInputPointerUp(
                panelWidgetId, localX, localY);
        if (!hostInputHandled &&
            PtInRect(&content, upPoint) &&
            widgetEngine_)
        {
            widgetEngine_->InvokeMouseEvent(
                panelWidgetId,
                "onPanelMouseUp",
                localX, localY, 1, 0);
            widgetEngine_->InvokeMouseEvent(
                panelWidgetId,
                "onPanelClick",
                localX, localY, 1, 0);
        }
        luaWidgetPanelMouseDown_ = false;
        mouseDown_ = false;
        ReleaseCapture();
        UpdateHostInputImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    for (auto& container : containers_)
    {
        auto* searchable =
            dynamic_cast<ScrollingItemWidget*>(container.get());
        if (!searchable ||
            !searchable->IsSearchPointerSelecting())
            continue;
        searchable->UpdateSearchPointerSelection(upPoint);
        searchable->EndSearchPointerSelection();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ =
            static_cast<size_t>(-1);
        ReleaseCapture();
        UpdateHostInputImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // ── Widget action completion ────────────────────────────
    if (widgetAction_ != WidgetAction::None && mouseDownWidgetIndex_ < widgets_.size())
    {
        if (widgetAction_ == WidgetAction::Move)
        {
            const auto movingPayload = snowdesktop::slot_contract::
                PayloadForWidgetType(
                    widgets_[mouseDownWidgetIndex_].type);
            const bool canCollectionGroup =
                widgetCollectionGroupTargetIndex_ < widgets_.size() &&
                widgets_[widgetCollectionGroupTargetIndex_].type ==
                    DesktopWidgetType::CollectionGroup &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::CollectionGroup,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            const bool canFileGroup =
                widgetCollectionGroupTargetIndex_ < widgets_.size() &&
                widgets_[widgetCollectionGroupTargetIndex_].type ==
                    DesktopWidgetType::FileGroup &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::FileGroup,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            const bool canGroup =
                canCollectionGroup || canFileGroup;
            DockContainer* dock = canGroup
                ? nullptr : GetDockContainerAtPoint(upPoint);
            const bool canDock = !canGroup && dock &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Dock,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            if (canGroup)
            {
                if (canCollectionGroup)
                    AddCollectionToGroup(
                        mouseDownWidgetIndex_,
                        widgetCollectionGroupTargetIndex_,
                        widgetCollectionGroupInsertIndex_);
                else
                    AddWidgetToFileGroup(
                        mouseDownWidgetIndex_,
                        widgetCollectionGroupTargetIndex_,
                        widgetCollectionGroupInsertIndex_);
            }
            else if (canDock)
            {
                Widget dockSource(&widgets_[mouseDownWidgetIndex_], this);
                int mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MK_CONTROL : 0;
                CommitDockDrop({ &dockSource }, nullptr, dock,
                    widgetDockInsertIndex_, mods);
                SaveLayoutSlots();
                RebuildContainersAndItems();
                LayoutItems();
            }
            else
                PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, true);
        }
        else if (widgetAction_ == WidgetAction::Resize)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, false);
        // PendingMove/PendingResize: just cancel without displacement
        widgetAction_ = WidgetAction::None;
        widgetDockTarget_ = false;
        widgetDockTargetContainer_ = nullptr;
        widgetDockInsertIndex_ = 0;
        widgetCollectionGroupTargetIndex_ =
            static_cast<size_t>(-1);
        widgetCollectionGroupInsertIndex_ =
            static_cast<size_t>(-1);
        InvalidateDragStaticScene();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragSession_.IsActive())
    {
        if (HandleDockClickRelease(upPoint))
            return;
        dockPressedEntry_ = static_cast<size_t>(-1);
        dockPressedFrequentItem_ = static_cast<size_t>(-1);
        dockPressedRunningAppKey_.clear();
        dockPressedWindowAction_ =
            snowdesktop::dock_window_rules::DockClickAction::None;
        dockPressedTargetWindow_ = nullptr;
        dockPressedContainer_ = nullptr;
        if (mouseDownWidgetIndex_ < widgets_.size() &&
            widgets_[mouseDownWidgetIndex_].type ==
                DesktopWidgetType::LuaScript &&
            widgetEngine_)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]);
            widgetEngine_->EnsureWidgetLoaded(widgets_[mouseDownWidgetIndex_].id,
                widgets_[mouseDownWidgetIndex_].packageId);
            const bool hostInputHandled =
                widgetEngine_->HandleHostInputPointerUp(
                    widgets_[mouseDownWidgetIndex_].id,
                    upPoint.x - frame.left,
                    upPoint.y - frame.top);
            if (hostInputHandled)
                UpdateHostInputImePosition();
            if (!hostInputHandled &&
                HitTestStandaloneWidget(mouseDownWidgetIndex_,
                    upPoint) == WidgetHit::Content)
            {
                widgetEngine_->InvokeMouseEvent(
                    widgets_[mouseDownWidgetIndex_].id,
                    "onMouseUp", upPoint.x - frame.left,
                    upPoint.y - frame.top, 1, 0);
                widgetEngine_->InvokeClick(
                    widgets_[mouseDownWidgetIndex_].id,
                    upPoint.x - frame.left,
                    upPoint.y - frame.top);
            }
        }
        if (pendingCtrlToggleDesktopIndex_ < items_.size())
            items_[pendingCtrlToggleDesktopIndex_].selected = false;
        pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
        if (pendingCtrlToggleWidgetItem_)
        {
            pendingCtrlToggleWidgetItem_->SetSelected(!pendingCtrlToggleWidgetItem_->IsSelected());
            pendingCtrlToggleWidgetItem_ = nullptr;
        }
        mouseDown_ = false;
        marqueeActive_ = false;
        marqueeWidgetIndex_ = static_cast<size_t>(-1);
        marqueeDockFolderPopup_ = false;
        dockFolderPopupMarqueeInitialSelection_.clear();
        navHoverSide_ = 0;
        navAutoFlipDir_ = 0;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Dock 项轻微拖动后仍落回原项时，按单击处理而不是吞掉本次操作。
    if (HandleDockClickRelease(upPoint))
        goto cleanup;

    if (!GetDockDragOutRemovalHint(upPoint).empty())
    {
        const bool removed = RemoveDockDragOutItems(dragSession_.Items());
        ClearSelection();
        EndDragSession();
        if (removed)
        {
            SaveLayoutSlots();
            RebuildContainersAndItems();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        goto cleanup;
    }

    if (!dragSession_.TargetContainer() ||
        dragSession_.TargetRegion() == HitRegion::None ||
        dragSession_.TargetRegion() == HitRegion::Blocked)
    {
        goto cleanup;
    }

    dropPreviewMods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        dropPreviewMods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        dropPreviewMods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        dropPreviewMods |= MK_SHIFT;
    commitVisualBeforeDrop =
        dragSession_.TargetRegion() == HitRegion::Handoff;
    if (!commitVisualBeforeDrop)
    {
        const DropPreviewList dropPreview = BuildDropPreviewList(
            dragSession_.SourceList(),
            dragSession_.TargetContainer(),
            dragSession_.TargetSlot(),
            dragSession_.TargetRegion(),
            dropPreviewMods,
            dragSession_.CurrentPoint());
        commitVisualBeforeDrop = dropPreview.fileBacked;
    }

    // Shell/file-backed drop handlers may synchronously show a progress
    // window. Commit their visual end up front, but keep pure internal moves
    // in the current frame until the model has its final position. Otherwise
    // the source item is synchronously painted once at its old location.
    dragSession_.DeactivateForDrop();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    ReleaseCapture();
    if (commitVisualBeforeDrop)
        CommitDragVisualEndBeforeShellOperation();

    {
        const int mods = dropPreviewMods;

        if (dragSession_.TargetRegion() == HitRegion::Handoff && dragSession_.TargetSlot()
            && dragSession_.TargetSlot()->GetItem())
        {
            Item* targetItem = dragSession_.TargetSlot()->GetItem();
            const bool dockFolderPopupTarget =
                dockFolderPopupOpen_ &&
                targetItem->GetContainer() ==
                    dockFolderPopupContainer_.get();
            const bool dockFolderPopupSource =
                dockFolderPopupOpen_ &&
                dragSession_.Source() ==
                    dockFolderPopupContainer_.get();
            bool explicitDockFolderTarget =
                dockFolderPopupTarget;
            if (auto* dockTarget = dynamic_cast<DockEntryItem*>(targetItem))
            {
                const size_t targetEntryIndex =
                    dockTarget->GetEntryIndex();
                explicitDockFolderTarget =
                    targetEntryIndex <
                        dockEntries_.size() &&
                    IsFolderDockEntry(
                        dockEntries_[
                            targetEntryIndex]);
                if (dynamic_cast<DockContainer*>(
                        dragSession_.Source()) &&
                    targetEntryIndex <
                        dockEntries_.size() &&
                    IsRecycleBinDockEntry(
                        dockEntries_[targetEntryIndex]) &&
                    RemoveDockDragOutItems(
                        dragSession_.Items()))
                {
                    ClearSelection();
                    EndDragSession();
                    SaveLayoutSlots();
                    RebuildContainersAndItems();
                    LayoutItems();
                    InvalidateRect(
                        hwnd_, nullptr, FALSE);
                    goto cleanup;
                }
                if (dockTarget->GetEntryType() == DockEntryType::Collection)
                {
                    const bool executed = DropItemsIntoDockCollection(
                        dragSession_.Items(), dragSession_.Source(), dockTarget, mods);
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    if (executed)
                    {
                        RebuildContainersAndItems();
                        LayoutItems();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    goto cleanup;
                }
            }
            auto* targetDesktopIcon = dynamic_cast<DesktopIcon*>(targetItem);
            DesktopItem* targetDesktopItem = targetDesktopIcon
                ? targetDesktopIcon->GetDesktopItem() : nullptr;
            if (dynamic_cast<DockContainer*>(dragSession_.Source()) && targetDesktopItem &&
                _wcsicmp(targetDesktopItem->desktopIconClsid.c_str(),
                    kDesktopIconClsidRecycleBin) == 0)
            {
                MoveDockItemsToDesktop(dragSession_.Items(),
                    CellFromPointForDrag(dragSession_.CurrentPoint()));
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                goto cleanup;
            }
            const std::vector<std::wstring> sourcePaths =
                dragSession_.SourceList().FilePaths();
            const bool recycleBinTarget =
                targetDesktopItem &&
                _wcsicmp(
                    targetDesktopItem->desktopIconClsid.c_str(),
                    kDesktopIconClsidRecycleBin) == 0;
            const std::wstring targetPath = targetItem->GetPath();
            const DWORD targetAttributes = targetPath.empty()
                ? INVALID_FILE_ATTRIBUTES
                : GetFileAttributesW(targetPath.c_str());
            if (!sourcePaths.empty() && recycleBinTarget)
            {
                std::vector<snowdesktop::ShellFileOperationStep> steps;
                steps.push_back({
                    FO_DELETE,
                    sourcePaths,
                    {},
                    static_cast<FILEOP_FLAGS>(
                        FOF_ALLOWUNDO |
                        FOF_NOCONFIRMATION) });
                QueueShellFileOperation(
                    std::move(steps),
                    [this,
                     dockFolderPopupTarget,
                     dockFolderPopupSource](bool succeeded) {
                        if (!succeeded)
                            return;
                        ReloadItems(false);
                        if ((dockFolderPopupTarget ||
                             dockFolderPopupSource) &&
                            dockFolderPopupOpen_)
                            RefreshDockFolderPopup();
                    });
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                goto cleanup;
            }
            if (!sourcePaths.empty() &&
                targetAttributes != INVALID_FILE_ATTRIBUTES &&
                (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                DWORD keyState = MK_LBUTTON;
                if (mods & MK_CONTROL) keyState |= MK_CONTROL;
                if (mods & MK_ALT) keyState |= MK_ALT;
                if (mods & MK_SHIFT) keyState |= MK_SHIFT;
                const DWORD selectedEffect = ChooseDropEffect(
                    keyState,
                    DROPEFFECT_COPY |
                        DROPEFFECT_MOVE |
                        DROPEFFECT_LINK);
                const DropAction action =
                    selectedEffect == DROPEFFECT_LINK
                        ? DropAction::Link
                        : selectedEffect == DROPEFFECT_COPY
                            ? DropAction::Copy
                            : DropAction::Move;
                DragSourceList fileSources =
                    dragSession_.SourceList();
                auto finished = [this,
                    dockFolderPopupTarget,
                    dockFolderPopupSource](bool succeeded) {
                    if (!succeeded)
                        return;
                    ReloadItems(false);
                    if ((dockFolderPopupTarget ||
                         dockFolderPopupSource) &&
                        dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                };
                if (action == DropAction::Link)
                {
                    const bool succeeded = MaterializeFilesToFolder(
                        fileSources, targetPath, action, {});
                    finished(succeeded);
                }
                else
                {
                    MaterializeFilesToFolder(
                        fileSources, targetPath, action,
                        std::move(finished));
                }
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                goto cleanup;
            }
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj)
            {
                ComPtr<IDropTarget> dropTarget;
                if (auto* targetIcon = dynamic_cast<DesktopIcon*>(targetItem))
                {
                    DesktopItem* desktopItem = targetIcon->GetDesktopItem();
                    if (desktopItem && desktopItem->childPidl.get())
                    {
                        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(desktopItem->childPidl.get());
                        desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                            reinterpret_cast<void**>(dropTarget.GetAddressOf()));
                    }
                }
                else if (!targetItem->GetPath().empty())
                {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                        nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                    {
                        shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dropTarget));
                    }
                }

                if (dropTarget)
                {
                    POINT screen = dragSession_.CurrentPoint();
                    ClientToScreen(hwnd_, &screen);
                    POINTL spl{ screen.x, screen.y };
                    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                    DWORD keyState = MK_LBUTTON;
                    if (mods & MK_CONTROL) keyState |= MK_CONTROL;
                    if (mods & MK_ALT)     keyState |= MK_ALT;
                    if (mods & MK_SHIFT)   keyState |= MK_SHIFT;
                    if (explicitDockFolderTarget &&
                        (mods & (MK_CONTROL |
                            MK_ALT | MK_SHIFT)) == 0)
                        keyState |= MK_SHIFT;
                    if (SUCCEEDED(dropTarget->DragEnter(dataObj.Get(), keyState, spl, &effect)))
                    {
                        dropTarget->DragOver(keyState, spl, &effect);
                        dropTarget->Drop(dataObj.Get(), keyState, spl, &effect);
                    }
                }
            }
            SaveLayoutSlots();
            ClearSelection();
            EndDragSession();
            if (dockFolderPopupTarget ||
                dockFolderPopupSource)
                RefreshDockFolderPopup();
            ReloadItems();
            goto cleanup;
        }

        Container* targetContainer = dragSession_.TargetContainer();
        bool needsReload = targetContainer->NeedsShellReloadAfterDrop();
        const bool dockFolderPopupTarget =
            dockFolderPopupOpen_ &&
            targetContainer ==
                dockFolderPopupContainer_.get();
        const bool dockFolderPopupSource =
            dockFolderPopupOpen_ &&
            dragSession_.Source() ==
                dockFolderPopupContainer_.get();
        targetContainer->OnItemsDropped(dragSession_.Items(), dragSession_.Source(),
            dragSession_.TargetSlot(), dragSession_.TargetRegion(), mods);

        SaveLayoutSlots();
        ClearSelection();
        EndDragSession();
        if (dockFolderPopupTarget ||
            dockFolderPopupSource)
            RefreshDockFolderPopup();
        if (needsReload)
        {
            RebuildContainersAndItems();
            ReloadItems();
        }
        else
        {
            // 内容变更可能使某些溢出页变空（后面有非空页时应立即清理顺延）
            // 先 ApplyPageMapping（可能重排 pageId），再 RebuildContainersAndItems + LayoutItems
            ApplyPageMapping();
            RebuildContainersAndItems();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

cleanup:
    EndDragSession();
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    popupDwellController_.Reset();
    KillTimer(hwnd_, kCollectionPopupDwellTimerId);
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    dockPressedContainer_ = nullptr;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    ReleaseCapture();
}
