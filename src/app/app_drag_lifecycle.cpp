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
        if (!compositionPaintInProgress_)
            UpdateWindow(hwnd_);
        else
        {
            // 同步绘制重入中不能再次 UpdateWindow，交给 scheduler 兜底补一帧。
            desktopPointerPresentPending_ = true;
            EnsureUiAnimationFrame();
        }
    }
    if (floatingDockVisible_)
    {
        const ULONGLONG now = GetTickCount64();
        const bool presentNow =
            snowdesktop::floating_dock_rules::
                ShouldPresentPointerFrame(
                    now,
                    floatingDockLastPointerPresentTick_,
                    immediateDesktopPresent);
        if (presentNow)
            floatingDockLastPointerPresentTick_ = now;
        InvalidateFloatingDockWindow(presentNow);
    }
}

/**
 * @brief 结束当前拖拽会话，重置拖拽渲染缓存
 */
void DesktopApp::EndDragSession()
{
    if (hwnd_)
    {
        KillTimer(hwnd_, kDockHandoffDwellTimerId);
        KillTimer(
            hwnd_, kCollectionGroupTabDwellTimerId);
    }
    dockHandoffDwellIndex_ = static_cast<size_t>(-1);
    dockHandoffDwellStartTick_ = 0;
    dockHandoffDwellReady_ = false;
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
}

void DesktopApp::PresentPassiveHoverVisualChange()
{
    // Content and backdrop are collected from the same full render pass. The
    // backdrop compositor constrains its helper HWND to the resulting panel
    // set, so stale blur pixels cannot outlive the content frame even if the
    // Windows Composition commit completes later.
    desktopBackdropFullCollectionPending_ = true;
    bool frameSubmitted = false;
    if (hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (!compositionPaintInProgress_)
        {
            UpdateWindow(hwnd_);
            frameSubmitted = true;
        }
    }
    if (frameSubmitted)
        DwmFlush();
    InvalidateFloatingDockWindow(true);
}

/**
 * @brief 显示设置窗口
 */
