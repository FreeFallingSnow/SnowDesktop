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
}

/**
 * @brief 同步提交快速拖动帧，避免连续 WM_MOUSEMOVE 让 WM_PAINT 饥饿。
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
    CommitPassiveHoverVisualEnd(true);
}

void DesktopApp::CommitPassiveHoverVisualEnd(
    bool forceCompositionFlush)
{
    bool backdropRemoved = false;
    for (size_t widgetIndex = 0;
         widgetIndex < widgets_.size();
         ++widgetIndex)
    {
        const DesktopWidget& widget = widgets_[widgetIndex];
        if (IsRectEmptyRect(widget.bounds))
            continue;
        const bool popupOpen =
            popupWidgetIndex_ == widgetIndex ||
            (!interactionPinnedWidgetId_.empty() &&
                interactionPinnedWidgetId_ == widget.id);
        const bool pointerInside =
            PtInRect(
                &widget.bounds,
                lastMousePoint_) != FALSE;
        if (snowdesktop::widget_visibility_rules::
                ShouldRetainBackdropAfterDrag(
                    widget.showOnHoverOnly,
                    widget.selected,
                    popupOpen,
                    pointerInside))
            continue;
        backdropRemoved =
            desktopBackdropCompositor_.RemovePanel(
                GetStandaloneWidgetFrameRect(widget)) ||
            backdropRemoved;
    }

    if (hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (!compositionPaintInProgress_)
            UpdateWindow(hwnd_);
    }
    // Hover-only backdrops live in a separate composition tree. Flush when a
    // panel was removed, or unconditionally before a Shell operation can enter
    // a nested progress loop, so the two visual trees cannot diverge.
    if (backdropRemoved || forceCompositionFlush)
        DwmFlush();
    InvalidateFloatingDockWindow(true);
}

/**
 * @brief 显示设置窗口
 */
