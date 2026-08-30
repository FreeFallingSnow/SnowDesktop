#include "app.h"
#include "../drag_visual_rules.h"
#include "../page_navigation_rules.h"
#include "../widget_visibility_rules.h"

// Drag-scene invalidation, presentation and session teardown.

bool DesktopApp::IsPointOverWidgetChrome(POINT pt) const
{
    for (auto& c : containers_)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(c.get()))
            continue;
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc) continue;
        RECT frame = wc->GetFrameRect();
        if (!IsRectEmptyRect(frame) && PtInRect(&frame, pt))
            return true;
    }
    return HitTestStandaloneWidgetIndex(pt) != static_cast<size_t>(-1);
}

/**
 * @brief 使拖拽静态场景失效（更新拖拽渲染缓存）
 */
void DesktopApp::InvalidateDragStaticScene()
{
    dragSession_.InvalidateStaticScene();
    dragRenderCache_.Reset();
    // 原生毛玻璃面板位于独立的 Composition 视觉树中，不包含在 D2D
    // 静态位图里。页面或布局变化时必须在下一帧完整核对一次，否则
    // 交互帧的“保留旧面板”策略会让上一页的玻璃区域残留。
    desktopBackdropFullCollectionPending_ = true;
}

/**
 * @brief 同步提交快速拖动帧，避免连续 WM_MOUSEMOVE 让 WM_PAINT 饥饿。
 *
 * 拖拽虚影、组件预览和 marquee 都属于指针反馈，必须在本消息内同步提交。
 * 不要改成 desktopPointerPresentPending_ + EnsureUiAnimationFrame()：
 * f29a882 曾这样改，密集鼠标事件下所有反馈都晚一帧。真实逐帧动画才走
 * UiAnimationScheduler。
 */
void DesktopApp::PresentDesktopPointerUpdate()
{
    if (handlingFloatingDockInput_ ||
        handlingFloatingPopupInput_ ||
        !hwnd_ || !IsWindow(hwnd_))
        return;
    if (!compositionPaintInProgress_)
    {
        UpdateWindow(hwnd_);
        return;
    }

    // A nested paint cannot re-enter D2D/DComp. This is the only pointer path
    // allowed to fall back to the scheduler; the normal path above presents
    // before the current input message returns.
    desktopPointerPresentPending_ = true;
    EnsureUiAnimationFrame();
}

void DesktopApp::PresentOleDragInteractionFrame()
{
    PresentPointerInteractionFrame();

    // A self drag reaches these callbacks from DoDragDrop's nested message
    // loop. The outer application pump therefore cannot perform its normal
    // end-of-message DComp flush until the complete drag has returned. Submit
    // this frame here so leave/re-enter hit feedback never remains frozen at
    // the last frame that was committed before crossing into another app.
    FlushPendingCompositionCommit();
}

void DesktopApp::PresentPointerInteractionFrame(
    bool dragPreviewAlreadySynced)
{
    // Move the cached compact drag surface before any desktop or Dock paint.
    // This keeps the ghost attached to the input message even when the
    // larger feedback surfaces need more time to redraw.
    if (snowdesktop::drag_visual_rules::
            ShouldSyncPreviewBeforePresentation(
                dragPreviewAlreadySynced))
        SyncDragPreviewWindow();
    RefreshDragPresentationAnchor();
    const bool widgetPreviewActive =
        widgetAction_ == WidgetAction::Move ||
        widgetAction_ == WidgetAction::Resize;
    const bool itemDragActive = dragSession_.IsActive();
    const bool pageNavDragActive =
        widgetAction_ == WidgetAction::Move ||
        itemDragActive ||
        dragDropController_.IsTransportActive();
    const int pageNavDragHintSide =
        pageNavDragActive && navHotEdgeHover_ &&
            (navHoverSide_ == -1 || navHoverSide_ == 1)
        ? navHoverSide_
        : 0;
    RECT pageNavDragHintBounds{};
    if (pageNavDragHintSide != 0)
    {
        pageNavDragHintBounds = GetPageNavHotEdgeHintBounds(
            pageNavDragHintSide, lastMousePoint_);
    }
    const bool pageNavDragHintChanged =
        snowdesktop::page_navigation_rules::NeedsDragHintPresent(
            pageNavDragActive,
            pageNavDragHintSide,
            pageNavDragHintBounds,
            presentedDragNavHintSide_,
            presentedDragNavHintBounds_);
    const std::uint64_t feedbackRevision =
        itemDragActive
            ? dragSession_.PresentationRevision()
            : 0;
    const bool itemDragFeedbackChanged =
        itemDragActive &&
        (feedbackRevision !=
             presentedDragFeedbackRevision_ ||
         navHoverSide_ !=
             presentedDragNavHoverSide_);
    if (itemDragActive)
    {
        presentedDragFeedbackRevision_ =
            feedbackRevision;
        presentedDragNavHoverSide_ = navHoverSide_;
    }
    else
    {
        presentedDragFeedbackRevision_ = 0;
        presentedDragNavHoverSide_ = 0;
    }
    snowdesktop::widget_composition_layer_rules::
        WidgetDragFeedbackState widgetDragFeedback{};
    widgetDragFeedback.active = widgetPreviewActive;
    widgetDragFeedback.resize =
        widgetAction_ == WidgetAction::Resize;
    widgetDragFeedback.pageId = widgetPreviewCell_.pageId;
    widgetDragFeedback.column = widgetPreviewCell_.column;
    widgetDragFeedback.row = widgetPreviewCell_.row;
    widgetDragFeedback.columns = widgetPreviewSpan_.columns;
    widgetDragFeedback.rows = widgetPreviewSpan_.rows;
    widgetDragFeedback.dockTarget = widgetDockTarget_;
    widgetDragFeedback.dockOwner = reinterpret_cast<std::uintptr_t>(
        widgetDockTargetContainer_);
    widgetDragFeedback.dockInsertIndex = widgetDockInsertIndex_;
    widgetDragFeedback.groupTargetIndex =
        widgetCollectionGroupTargetIndex_;
    widgetDragFeedback.groupInsertIndex =
        widgetCollectionGroupInsertIndex_;
    widgetDragFeedback.navigationSide = navHoverSide_;
    const bool widgetDragFeedbackChanged =
        snowdesktop::widget_composition_layer_rules::
            NeedsWidgetDragFeedbackPresent(
                presentedWidgetDragFeedback_,
                widgetDragFeedback);
    if (!widgetPreviewActive)
        presentedWidgetDragFeedback_ = {};
    const bool immediateDesktopPresent =
        snowdesktop::floating_dock_rules::
            NeedsImmediatePointerPresent(
                itemDragFeedbackChanged,
                widgetPreviewActive,
                marqueeActive_);
    bool widgetInteractionPresented = false;
    if (widgetPreviewActive && hwnd_ && IsWindow(hwnd_))
    {
        if (widgetDragFeedbackChanged)
        {
            // Grid, Dock and group feedback lives on the shared foreground
            // surface. Redraw it only when the logical target changes; plain
            // pointer movement inside one target has no visual work.
            RECT client{};
            GetClientRect(hwnd_, &client);
            widgetInteractionPresented =
                PresentDesktopForegroundComposition(client);
        }
        else
        {
            widgetInteractionPresented = true;
        }
    }
    bool marqueeInteractionPresented = false;
    if (marqueeActive_ && !marqueeFullPresentPending_ &&
        hwnd_ && IsWindow(hwnd_))
    {
        // The first marquee frame still repaints the complete desktop so
        // selected icon pixels move from the background to the interaction
        // layer. Later frames change only that layer (and, when applicable,
        // the one widget surface queued by OnMouseMoveAt).
        RECT client{};
        GetClientRect(hwnd_, &client);
        marqueeInteractionPresented =
            PresentDesktopForegroundComposition(client);
    }
    bool desktopFallbackPresented = false;
    if (immediateDesktopPresent &&
        !marqueeInteractionPresented &&
        (!widgetPreviewActive || !widgetInteractionPresented) &&
        hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        PresentDesktopPointerUpdate();
        desktopFallbackPresented = true;
    }
    if (marqueeActive_ && marqueeFullPresentPending_ &&
        desktopFallbackPresented)
    {
        marqueeFullPresentPending_ = false;
    }
    bool pageNavDragHintPresented =
        pageNavDragHintChanged &&
        ((widgetPreviewActive && widgetDragFeedbackChanged &&
             widgetInteractionPresented) ||
            desktopFallbackPresented);
    if (pageNavDragHintChanged &&
        !pageNavDragHintPresented &&
        hwnd_ && IsWindow(hwnd_))
    {
        RECT dirty{};
        bool hasDirty = false;
        const auto addDirty = [&](const RECT& bounds) {
            if (IsRectEmptyRect(bounds))
                return;
            if (!hasDirty)
            {
                dirty = bounds;
                hasDirty = true;
            }
            else
            {
                RECT merged{};
                UnionRect(&merged, &dirty, &bounds);
                dirty = merged;
            }
        };
        addDirty(presentedDragNavHintBounds_);
        addDirty(pageNavDragHintBounds);
        if (hasDirty)
        {
            InflateRect(&dirty, 4, 4);
            pageNavDragHintPresented =
                PresentDesktopForegroundComposition(dirty);
            if (!pageNavDragHintPresented)
            {
                InvalidateRect(hwnd_, &dirty, FALSE);
                PresentDesktopPointerUpdate();
                pageNavDragHintPresented = true;
                desktopFallbackPresented = true;
            }
        }
        else
        {
            pageNavDragHintPresented = true;
        }
    }
    if (pageNavDragHintChanged && pageNavDragHintPresented)
    {
        presentedDragNavHintSide_ = pageNavDragHintSide;
        presentedDragNavHintBounds_ = pageNavDragHintBounds;
    }
    if (widgetPreviewActive && widgetDragFeedbackChanged &&
        (widgetInteractionPresented || desktopFallbackPresented))
    {
        presentedWidgetDragFeedback_ =
            std::move(widgetDragFeedback);
    }
    const bool immediateFloatingDockPresent =
        immediateDesktopPresent &&
        (!widgetPreviewActive || widgetDragFeedbackChanged);
    if (floatingDockHostActive_)
    {
        const void* hoverOwner = nullptr;
        size_t hoverIndex = 0;
        int hoverKind = 0;
        if (DockContainer* dock =
                GetDockContainerAtPoint(lastMousePoint_);
            dock && dock->ContainsInteractivePoint(
                lastMousePoint_))
        {
            hoverOwner = dock;
            if (DockEntryItem* entry =
                    dock->EntryAtPoint(lastMousePoint_))
            {
                hoverKind = 1;
                hoverIndex = entry->GetEntryIndex();
            }
            else if (DockRunningItem* running =
                    dock->RunningItemAtPoint(lastMousePoint_))
            {
                hoverKind = 2;
                hoverIndex = running->GetRunningIndex();
            }
            else if (DockFrequentItem* frequent =
                    dock->FrequentItemAtPoint(lastMousePoint_))
            {
                hoverKind = 3;
                hoverIndex = frequent->GetItemIndex();
            }
            else if (dock->IsWindowsButtonPoint(lastMousePoint_))
                hoverKind = 4;
            else if (dock->IsSearchPoint(lastMousePoint_))
                hoverKind = 5;
            else
                hoverKind = 6;
        }
        const bool hoverTargetChanged =
            hoverOwner != floatingDockHoverTargetOwner_ ||
            hoverIndex != floatingDockHoverTargetIndex_ ||
            hoverKind != floatingDockHoverTargetKind_;
        floatingDockHoverTargetOwner_ = hoverOwner;
        floatingDockHoverTargetIndex_ = hoverIndex;
        floatingDockHoverTargetKind_ = hoverKind;

        const ULONGLONG now = GetTickCount64();
        const bool presentNow =
            snowdesktop::floating_dock_rules::
                ShouldPresentPointerFrame(
                    now,
                    floatingDockLastPointerPresentTick_,
                    immediateFloatingDockPresent ||
                        hoverTargetChanged);
        if (presentNow)
        {
            if (floatingDockHoverTailToken_)
            {
                uiAnimationScheduler_.Cancel(
                    floatingDockHoverTailToken_);
                floatingDockHoverTailToken_ = 0;
            }
            floatingDockLastPointerPresentTick_ = now;
            InvalidateFloatingDockWindow(true);
        }
        else if (!floatingDockHoverTailToken_)
        {
            const UINT delay = std::max<UINT>(
                1,
                snowdesktop::floating_dock_rules::
                    RemainingPointerFrameDelay(
                        now,
                        floatingDockLastPointerPresentTick_));
            floatingDockHoverTailToken_ =
                uiAnimationScheduler_.ScheduleOnce(
                    delay,
                    [this](snowdesktop::UiScheduleToken token) {
                        if (floatingDockHoverTailToken_ != token)
                            return;
                        floatingDockHoverTailToken_ = 0;
                        if (!floatingDockHostActive_ ||
                            !floatingDockHwnd_ ||
                            !IsWindow(floatingDockHwnd_))
                            return;
                        floatingDockLastPointerPresentTick_ =
                            GetTickCount64();
                        InvalidateFloatingDockWindow(true);
                    });
        }
    }
    if (ShouldShowFloatingPopupWindow() &&
        (!itemDragActive ||
         itemDragFeedbackChanged) &&
        (!widgetPreviewActive ||
         widgetDragFeedbackChanged))
        InvalidateFloatingPopupWindow(true);
}

/**
 * @brief 停止 Dock 驻留计时并清空当前驻留目标。
 */
void DesktopApp::ResetDockHandoffDwell()
{
    if (snowdesktop::dock_drop_rules::IsDockHandoffDwellIdle(
            dockHandoffDwellIndex_,
            dockHandoffDwellStartTick_,
            dockHandoffDwellReady_))
        return;
    if (hwnd_)
        KillTimer(hwnd_, kDockHandoffDwellTimerId);
    dockHandoffDwellIndex_ = static_cast<size_t>(-1);
    dockHandoffDwellStartTick_ = 0;
    dockHandoffDwellReady_ = false;
}

void DesktopApp::ResetCompactCollectionHandoffDwell()
{
    if (compactCollectionHandoffWidgetId_.empty() &&
        compactCollectionHandoffIndex_ == static_cast<size_t>(-1) &&
        compactCollectionHandoffStartTick_ == 0 &&
        !compactCollectionHandoffReady_)
        return;
    if (hwnd_)
        KillTimer(hwnd_, kCompactCollectionHandoffDwellTimerId);
    compactCollectionHandoffWidgetId_.clear();
    compactCollectionHandoffIndex_ = static_cast<size_t>(-1);
    compactCollectionHandoffStartTick_ = 0;
    compactCollectionHandoffReady_ = false;
}

/**
 * @brief 解除会话对弹窗临时目标的引用并释放其成员包装对象。
 *
 * DeactivateForDrop 会保留目标用于同步提交，因此这里不能只检查活动态。
 */
void DesktopApp::ClearPopupDragTarget()
{
    Slot* popupTarget = popupDragTarget_.Get();
    if (popupTarget &&
        dragSession_.TargetSlot() == popupTarget)
    {
        dragSession_.UpdateTarget(
            nullptr, nullptr, HitRegion::None);
    }
    popupDragTarget_.Reset();
}

/**
 * @brief 结束当前拖拽会话，重置拖拽渲染缓存
 */
void DesktopApp::EndDragSession()
{
    ResetDockHandoffDwell();
    ResetCompactCollectionHandoffDwell();
    CancelCollectionPopupDwell();
    CancelCollectionGroupTabDwell();
    dragSession_.End();
    ClearPopupDragTarget();
    presentedDragFeedbackRevision_ = 0;
    presentedDragNavHoverSide_ = 0;
    HideDragPreviewWindow();
    ClearDockFolderPopupDragSourceSnapshot();
    dragRenderCache_.Reset();
    // 清除拖放预览缓存
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
    cachedDropPreviewTarget_ = nullptr;
    cachedDropPreviewSlot_ = nullptr;
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::ClearDockPressedState()
{
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    dockPressedContainer_ = nullptr;
    dockPressedClosedCollectionPopup_ = false;
}

bool DesktopApp::HasCancelablePointerPressState() const
{
    return mouseDown_ || mouseDownHit_ != nullptr ||
        dragSession_.IsActive() ||
        dockPressedEntry_ != static_cast<size_t>(-1) ||
        dockPressedFrequentItem_ != static_cast<size_t>(-1) ||
        !dockPressedRunningAppKey_.empty() ||
        dockPressedWindowAction_ !=
            snowdesktop::dock_window_rules::DockClickAction::None ||
        dockPressedTargetWindow_ != nullptr ||
        dockPressedContainer_ != nullptr ||
        dockPressedClosedCollectionPopup_ ||
        pendingGuideAction_ != WidgetHit::None ||
        marqueeActive_ ||
        widgetAction_ != WidgetAction::None ||
        middleButtonWidgetMove_ ||
        detailColumnResizeActive_ ||
        widgetScrollbarDragging_ ||
        popupScrollbarDragging_ ||
        luaWidgetPanelMouseDown_ ||
        popupMouseDownItem_ != nullptr;
}

bool DesktopApp::IsOwnedPointerCaptureWindow(HWND window) const
{
    return window &&
        (window == hwnd_ ||
         IsPersistentDockHostWindow(window) ||
         window == floatingPopupHwnd_);
}

bool DesktopApp::CanCancelPointerPressAfterCaptureLoss() const
{
    const bool retainedDropCommit =
        dragSession_.HasContext() &&
        !dragSession_.IsActive();
    return expectedCaptureReleaseDepth_ == 0 &&
        !dragDropController_.IsTransportActive() &&
        !retainedDropCommit &&
        HasCancelablePointerPressState();
}

void DesktopApp::CancelPointerPressWithoutCaptureRelease()
{
    const bool layoutNeedsSave =
        widgetScrollbarDragging_ ||
        detailColumnResizeActive_;
    const bool commitDockFolderPopupResize =
        detailColumnResizeActive_ &&
        detailColumnResizePopup_ &&
        dockFolderPopupOpen_;
    const bool dockItemPressed = dockPressedContainer_ &&
        (dockPressedEntry_ != static_cast<size_t>(-1) ||
         dockPressedFrequentItem_ != static_cast<size_t>(-1) ||
         !dockPressedRunningAppKey_.empty());
    Item* const pressedDockItem =
        dockItemPressed ? mouseDownHit_ : nullptr;
    HideDragHintWindow();
    SetPageNavHotEdgeHover(0);
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    ResetDockHandoffDwell();
    ResetCompactCollectionHandoffDwell();
    CancelCollectionPopupDwell();
    CancelCollectionGroupTabDwell();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleDesktopIndex_ =
        static_cast<size_t>(-1);
    pendingCtrlToggleWidgetIndex_ =
        static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    pendingGuideAction_ = WidgetHit::None;
    if (pressedDockItem)
        pressedDockItem->SetSelected(false);
    ClearDockPressedState();
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    for (auto& container : containers_)
    {
        if (auto* searchable =
                dynamic_cast<ScrollingItemWidget*>(container.get());
            searchable && searchable->IsSearchPointerSelecting())
        {
            searchable->EndSearchPointerSelection();
        }
    }
    widgetAction_ = WidgetAction::None;
    middleButtonWidgetMove_ = false;
    detailColumnResizeActive_ = false;
    detailColumnResizePopup_ = false;
    detailColumnResizeColumn_ =
        snowdesktop::list_detail_rules::Column::None;
    detailColumnResizeHeaderLeft_ = 0;
    detailColumnResizeHeaderWidth_ = 1;
    widgetScrollbarDragging_ = false;
    widgetScrollbarDragContainer_ = nullptr;
    popupScrollbarDragging_ = false;
    luaWidgetPanelMouseDown_ = false;
    widgetDockTarget_ = false;
    widgetDockTargetContainer_ = nullptr;
    widgetDockInsertIndex_ = 0;
    widgetCollectionGroupTargetIndex_ =
        static_cast<size_t>(-1);
    widgetCollectionGroupInsertIndex_ =
        static_cast<size_t>(-1);
    if (dragSession_.IsActive())
        EndDragSession();
    // End the session before destroying popup-owned Item/Slot wrappers that
    // may still be referenced by its source or target lists.
    ClearPopupMouseDownItem();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    if (widgetEngine_)
        widgetEngine_->CancelInteractionPointerPress();
    if (commitDockFolderPopupResize)
        CommitDockFolderPopupStateToSource();
    else if (layoutNeedsSave)
        SaveLayoutSlots();
    InvalidateDragStaticScene();
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateFloatingDockWindow();
    InvalidateFloatingPopupWindow();
}

void DesktopApp::ReleaseCapturePreservingPointerState()
{
    ++expectedCaptureReleaseDepth_;
    ReleaseCapture();
    --expectedCaptureReleaseDepth_;
}

void DesktopApp::CancelActiveItemDrag()
{
    CancelPointerPressWithoutCaptureRelease();
    ReleaseCapture();
}

void DesktopApp::CommitDragVisualEndBeforeShellOperation()
{
    HideDragPreviewWindow();
    dragRenderCache_.Reset();
    PresentPassiveHoverVisualChange();
    // The ordinary hover path must never wait for DWM. Shell operations are a
    // presentation boundary, but asynchronous handlers only need the batched
    // DComp work submitted before they are queued. Synchronous compatibility
    // fallbacks perform their own DwmFlush immediately before entering Shell.
    FlushPendingCompositionCommit();
}

void DesktopApp::PresentPassiveHoverVisualChange()
{
    RecordShellHoverTrace(
        ShellHoverTraceEvent::PassivePresent);
    // Content and backdrop are collected from the same full render pass. The
    // backdrop compositor constrains its helper HWND to the resulting panel
    // set, so stale blur pixels cannot outlive the content frame even if the
    // Windows Composition commit completes later.
    desktopBackdropFullCollectionPending_ = true;
    if (hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (!compositionPaintInProgress_)
            UpdateWindow(hwnd_);
    }
    InvalidateFloatingDockWindow(true);
    InvalidateFloatingPopupWindow(true);
}

/**
 * @brief 显示设置窗口
 */
