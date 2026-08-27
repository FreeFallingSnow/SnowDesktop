#include "app.h"

namespace
{
std::uint64_t AnimationTick(double nowMilliseconds)
{
    return static_cast<std::uint64_t>(
        std::max(0.0, std::floor(nowMilliseconds)));
}
}

// Every independently visible object owns its scheduler token. A terminal
// callback can therefore retire only its own track; it cannot accidentally
// stop hover, another popup, or a transition that started in the same frame.
void DesktopApp::EnsureUiAnimationFrame()
{
    if (!popupAnimationFrameToken_ &&
        popupAnimation_.IsAnimating() &&
        !popupAnimationCompositorDriven_)
    {
        popupAnimationFrameToken_ =
            uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::Popup,
                [this](double nowMilliseconds) {
                    popupAnimation_.Advance(
                        AnimationTick(nowMilliseconds));
                    if (!popupAnimation_.IsAnimating() &&
                        popupAnimation_.IsHidden())
                    {
                        FinalizeCloseCollectionPopup();
                    }
                    else if (!popupAnimation_.IsAnimating())
                    {
                        RECT dirty = popupAnimationCacheRect_;
                        PrepareCompositionAnimationOverlayRetirement(
                            popupAnimationOverlay_, dirty);
                        ResetCollectionPopupAnimationCache();
                        if (hwnd_ && IsWindow(hwnd_))
                            InvalidateRect(hwnd_, &dirty, FALSE);
                        UpdateFloatingPopupWindowBounds(true);
                    }
                    else if (UpdateCollectionPopupCompositionAnimation(
                            false))
                    {
                        CommitCompositionAnimationFrame();
                    }
                    else
                    {
                        InvalidateCollectionPopupAnimation();
                    }

                    const bool keep =
                        popupAnimation_.IsAnimating() &&
                        !popupAnimationCompositorDriven_;
                    if (!keep)
                        popupAnimationFrameToken_ = 0;
                    return keep;
                });
    }

    if (!luaPanelAnimationFrameToken_ &&
        luaWidgetPanelAnimation_.IsAnimating() &&
        !luaWidgetPanelAnimationCompositorDriven_)
    {
        luaPanelAnimationFrameToken_ =
            uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::Popup,
                [this](double nowMilliseconds) {
                    luaWidgetPanelAnimation_.Advance(
                        AnimationTick(nowMilliseconds));
                    if (!luaWidgetPanelAnimation_.IsAnimating() &&
                        luaWidgetPanelAnimation_.IsHidden())
                    {
                        FinalizeCloseLuaWidgetPanel();
                    }
                    else if (hwnd_ && IsWindow(hwnd_))
                    {
                        if (!luaWidgetPanelAnimation_.IsAnimating())
                        {
                            RECT dirty =
                                luaWidgetPanelAnimationCacheRect_;
                            PrepareCompositionAnimationOverlayRetirement(
                                luaWidgetPanelAnimationOverlay_, dirty);
                            ResetLuaWidgetPanelAnimationCache();
                            InvalidateRect(hwnd_, &dirty, FALSE);
                            UpdateFloatingPopupWindowBounds(true);
                        }
                        else if (
                            UpdateLuaWidgetPanelCompositionAnimation(false))
                        {
                            CommitCompositionAnimationFrame();
                        }
                        else
                        {
                            InvalidateFloatingPopupWindow(true);
                        }
                    }

                    const bool keep =
                        luaWidgetPanelAnimation_.IsAnimating() &&
                        !luaWidgetPanelAnimationCompositorDriven_;
                    if (!keep)
                        luaPanelAnimationFrameToken_ = 0;
                    return keep;
                });
    }

    if (!quickNavigationAnimationFrameToken_ &&
        quickNavigationAnimation_.IsAnimating() &&
        !quickNavigationAnimationCompositorDriven_)
    {
        quickNavigationAnimationFrameToken_ =
            uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::QuickNavigation,
                [this](double nowMilliseconds) {
                    quickNavigationAnimation_.Advance(
                        AnimationTick(nowMilliseconds));
                    ApplyQuickNavigationAnimationFrame();
                    if (!quickNavigationAnimation_.IsAnimating() &&
                        quickNavigationAnimation_.IsHidden())
                    {
                        FinalizeCloseQuickNavigation();
                    }
                    const bool keep =
                        quickNavigationAnimation_.IsAnimating();
                    if (!keep)
                        quickNavigationAnimationFrameToken_ = 0;
                    return keep;
                });
    }

    if (!dockBounceAnimationFrameToken_ &&
        !dockLaunchBounces_.empty())
    {
        dockBounceAnimationFrameToken_ =
            uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::FloatingDock,
                [this](double) {
                    OnDockLaunchBounceTimer();
                    const bool keep = !dockLaunchBounces_.empty();
                    if (!keep)
                        dockBounceAnimationFrameToken_ = 0;
                    return keep;
                });
    }

    const DWORD pageElapsed = pageNotifyActive_
        ? GetTickCount() - pageNotifyStartTick_
        : 0;
    const bool pageFadeActive = pageNotifyActive_ &&
        !pageNotifyCompositorDriven_ &&
        (pageElapsed < kPageNotifyFadeMs ||
         pageElapsed >= kPageNotifyVisibleMs - kPageNotifyFadeMs);
    if (!pageNotifyAnimationFrameToken_ && pageFadeActive)
    {
        pageNotifyAnimationFrameToken_ =
            uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::Desktop,
                [this](double) {
                    if (!pageNotifyActive_ ||
                        pageNotifyCompositorDriven_)
                    {
                        pageNotifyAnimationFrameToken_ = 0;
                        return false;
                    }

                    const DWORD elapsed =
                        GetTickCount() - pageNotifyStartTick_;
                    const RECT dirty = GetPageNotifyBounds();
                    if (elapsed >= kPageNotifyVisibleMs)
                    {
                        pageNotifyActive_ = false;
                        pageNotifyText_.clear();
                        ResetPageNotifyTextCache();
                        if (hwnd_ && IsWindow(hwnd_))
                        {
                            InvalidateRect(
                                hwnd_, IsRectEmpty(&dirty)
                                    ? nullptr : &dirty,
                                FALSE);
                        }
                    }
                    else if (hwnd_ && IsWindow(hwnd_))
                    {
                        float opacity = 1.0f;
                        if (elapsed < kPageNotifyFadeMs)
                        {
                            opacity = static_cast<float>(elapsed) /
                                static_cast<float>(kPageNotifyFadeMs);
                        }
                        else if (elapsed >=
                            kPageNotifyVisibleMs - kPageNotifyFadeMs)
                        {
                            opacity = static_cast<float>(
                                kPageNotifyVisibleMs - elapsed) /
                                static_cast<float>(kPageNotifyFadeMs);
                        }
                        if (UpdatePageNotifyCompositionAnimation(
                                opacity, false))
                        {
                            CommitCompositionAnimationFrame();
                        }
                        else
                        {
                            InvalidateRect(
                                hwnd_, IsRectEmpty(&dirty)
                                    ? nullptr : &dirty,
                                FALSE);
                        }
                    }

                    const DWORD currentElapsed = pageNotifyActive_
                        ? GetTickCount() - pageNotifyStartTick_
                        : 0;
                    const bool keep = pageNotifyActive_ &&
                        !pageNotifyCompositorDriven_ &&
                        (currentElapsed < kPageNotifyFadeMs ||
                         currentElapsed >= kPageNotifyVisibleMs -
                            kPageNotifyFadeMs);
                    if (!keep)
                        pageNotifyAnimationFrameToken_ = 0;
                    return keep;
                });
    }

    if (!pointerRecoveryFrameToken_ &&
        (desktopPointerPresentPending_ ||
         floatingDockPointerPresentPending_))
    {
        pointerRecoveryFrameToken_ =
            uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::FloatingDock,
                [this](double) {
                    if (desktopPointerPresentPending_ &&
                        (!hwnd_ || !IsWindow(hwnd_)))
                        desktopPointerPresentPending_ = false;
                    if (desktopPointerPresentPending_ &&
                        hwnd_ && IsWindow(hwnd_) &&
                        !compositionPaintInProgress_)
                    {
                        desktopPointerPresentPending_ = false;
                        RECT update{};
                        if (GetUpdateRect(hwnd_, &update, FALSE))
                        {
                            ValidateRect(hwnd_, &update);
                            OnPaint(&update);
                        }
                    }

                    if (floatingDockPointerPresentPending_ &&
                        (!floatingDockHostActive_ ||
                         !floatingDockHwnd_ ||
                         !IsWindow(floatingDockHwnd_)))
                    {
                        floatingDockPointerPresentPending_ = false;
                    }
                    if (floatingDockPointerPresentPending_ &&
                        floatingDockHostActive_ &&
                        floatingDockHwnd_ &&
                        IsWindow(floatingDockHwnd_) &&
                        !floatingDockCompositionPaintInProgress_)
                    {
                        floatingDockPointerPresentPending_ = false;
                        RECT update{};
                        if (GetUpdateRect(
                                floatingDockHwnd_, &update, FALSE))
                        {
                            ValidateRect(floatingDockHwnd_, &update);
                            if (!RenderFloatingDockCompositionFrame())
                            {
                                InvalidateRect(
                                    floatingDockHwnd_, nullptr, FALSE);
                                floatingDockPointerPresentPending_ = true;
                            }
                        }
                    }

                    const bool keep =
                        desktopPointerPresentPending_ ||
                        floatingDockPointerPresentPending_;
                    if (!keep)
                        pointerRecoveryFrameToken_ = 0;
                    return keep;
                });
    }
}

void DesktopApp::CancelUiAnimationFrame()
{
    snowdesktop::UiScheduleToken* const tracks[] = {
        &popupAnimationFrameToken_,
        &luaPanelAnimationFrameToken_,
        &quickNavigationAnimationFrameToken_,
        &dockBounceAnimationFrameToken_,
        &pageNotifyAnimationFrameToken_,
        &pointerRecoveryFrameToken_,
        &floatingDockHoverTailToken_,
    };
    for (snowdesktop::UiScheduleToken* track : tracks)
    {
        if (*track)
            uiAnimationScheduler_.Cancel(*track);
        *track = 0;
    }
}
