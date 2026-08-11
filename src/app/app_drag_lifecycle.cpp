#include "app.h"
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
    OnPaint();
    InvalidateFloatingDockWindow(true);

    // A self drag reaches these callbacks from DoDragDrop's nested message
    // loop. The outer application pump therefore cannot perform its normal
    // end-of-message DComp flush until the complete drag has returned. Submit
    // this frame here so leave/re-enter hit feedback never remains frozen at
    // the last frame that was committed before crossing into another app.
    FlushPendingCompositionCommit();
}

void DesktopApp::PresentPointerInteractionFrame()
{
    const bool widgetPreviewActive =
        widgetAction_ == WidgetAction::Move ||
        widgetAction_ == WidgetAction::Resize;
    const bool immediateDesktopPresent =
        snowdesktop::floating_dock_rules::
            NeedsImmediatePointerPresent(
                dragSession_.IsActive(),
                widgetPreviewActive,
                marqueeActive_);
    if (immediateDesktopPresent &&
        hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        PresentDesktopPointerUpdate();
    }
    if (floatingDockVisible_)
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
                    immediateDesktopPresent ||
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
                        if (!floatingDockVisible_ ||
                            floatingDockClosePending_ ||
                            !floatingDockHwnd_ ||
                            !IsWindow(floatingDockHwnd_))
                            return;
                        floatingDockLastPointerPresentTick_ =
                            GetTickCount64();
                        InvalidateFloatingDockWindow(true);
                    });
        }
    }
}

/**
 * @brief 停止 Dock 驻留计时并清空当前驻留目标。
 */
void DesktopApp::ResetDockHandoffDwell()
{
    if (hwnd_)
        KillTimer(hwnd_, kDockHandoffDwellTimerId);
    dockHandoffDwellIndex_ = static_cast<size_t>(-1);
    dockHandoffDwellStartTick_ = 0;
    dockHandoffDwellReady_ = false;
}

/**
 * @brief 结束当前拖拽会话，重置拖拽渲染缓存
 */
void DesktopApp::EndDragSession()
{
    ResetDockHandoffDwell();
    if (hwnd_)
    {
        KillTimer(
            hwnd_, kCollectionGroupTabDwellTimerId);
    }
    collectionGroupTabDwellWidgetIndex_ =
        static_cast<size_t>(-1);
    collectionGroupTabDwellId_.clear();
    collectionGroupTabDwellTick_ = 0;
    dragSession_.End();
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

void DesktopApp::CommitDragVisualEndBeforeShellOperation()
{
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
}

/**
 * @brief 显示设置窗口
 */
