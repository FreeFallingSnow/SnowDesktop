#include "app.h"

namespace
{
std::uint64_t AnimationTick(double nowMilliseconds)
{
    return static_cast<std::uint64_t>(
        std::max(0.0, std::floor(nowMilliseconds)));
}
}

void DesktopApp::EnsureUiAnimationFrame()
{
    if (uiAnimationFrameToken_)
        return;
    uiAnimationFrameToken_ = uiAnimationScheduler_.StartAnimation(
        snowdesktop::UiAnimationSurface::Desktop,
        [this](double nowMilliseconds) {
            return AdvanceUiAnimationFrame(nowMilliseconds);
        });
}

void DesktopApp::CancelUiAnimationFrame()
{
    if (uiAnimationFrameToken_)
        uiAnimationScheduler_.Cancel(uiAnimationFrameToken_);
    uiAnimationFrameToken_ = 0;
}

bool DesktopApp::AdvanceUiAnimationFrame(double nowMilliseconds)
{
    const std::uint64_t tick = AnimationTick(nowMilliseconds);
    bool desktopCompositionChanged = false;

    if (popupAnimation_.IsAnimating() &&
        !popupAnimationCompositorDriven_)
    {
        popupAnimation_.Advance(tick);
        if (!popupAnimation_.IsAnimating() &&
            popupAnimation_.IsHidden())
        {
            FinalizeCloseCollectionPopup();
        }
        else if (!popupAnimation_.IsAnimating())
        {
            RECT dirty = popupAnimationCacheRect_;
            ResetCollectionPopupAnimationCache();
            if (hwnd_ && IsWindow(hwnd_))
                InvalidateRect(hwnd_, &dirty, FALSE);
        }
        else if (UpdateCollectionPopupCompositionAnimation(false))
        {
            desktopCompositionChanged = true;
        }
        else
        {
            InvalidateCollectionPopupAnimation();
        }
    }

    if (luaWidgetPanelAnimation_.IsAnimating() &&
        !luaWidgetPanelAnimationCompositorDriven_)
    {
        luaWidgetPanelAnimation_.Advance(tick);
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
                ResetLuaWidgetPanelAnimationCache();
                InvalidateRect(hwnd_, &dirty, FALSE);
            }
            else if (UpdateLuaWidgetPanelCompositionAnimation(false))
            {
                desktopCompositionChanged = true;
            }
            else
            {
                RECT dirty = GetLuaWidgetPanelRect();
                InflateRect(&dirty, 3, 3);
                InvalidateRect(hwnd_, &dirty, FALSE);
            }
        }
    }

    if (quickNavigationAnimation_.IsAnimating())
    {
        quickNavigationAnimation_.Advance(tick);
        ApplyQuickNavigationAnimationFrame();
        if (!quickNavigationAnimation_.IsAnimating() &&
            quickNavigationAnimation_.IsHidden())
        {
            FinalizeCloseQuickNavigation();
        }
    }

    if (!dockLaunchBounces_.empty())
        OnDockLaunchBounceTimer();

    if (pageNotifyActive_ &&
        !pageNotifyCompositorDriven_)
    {
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
                    hwnd_, IsRectEmpty(&dirty) ? nullptr : &dirty,
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
            if (UpdatePageNotifyCompositionAnimation(opacity, false))
            {
                desktopCompositionChanged = true;
            }
            else
            {
                InvalidateRect(
                    hwnd_, IsRectEmpty(&dirty) ? nullptr : &dirty,
                    FALSE);
            }
        }
    }

    bool desktopFrameSubmitted = false;
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
            desktopFrameSubmitted = true;
        }
    }

    if (floatingDockPointerPresentPending_ &&
        (!floatingDockVisible_ ||
         !floatingDockHwnd_ ||
         !IsWindow(floatingDockHwnd_)))
        floatingDockPointerPresentPending_ = false;
    if (floatingDockPointerPresentPending_ &&
        floatingDockVisible_ &&
        floatingDockHwnd_ && IsWindow(floatingDockHwnd_) &&
        !floatingDockCompositionPaintInProgress_)
    {
        floatingDockPointerPresentPending_ = false;
        RECT update{};
        if (GetUpdateRect(floatingDockHwnd_, &update, FALSE))
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

    if (desktopCompositionChanged &&
        !desktopFrameSubmitted &&
        !CommitCompositionAnimationFrame())
    {
        // A failed property-only commit falls back to the already recorded
        // caches on the desktop surface. Keep the animation state itself so
        // reversing and terminal cleanup retain their existing semantics.
        ResetCompositionAnimationOverlay(popupAnimationOverlay_);
        ResetCompositionAnimationOverlay(
            luaWidgetPanelAnimationOverlay_);
        ResetCompositionAnimationOverlay(pageNotifyAnimationOverlay_);
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

    const DWORD pageElapsed = pageNotifyActive_
        ? GetTickCount() - pageNotifyStartTick_
        : 0;
    const bool pageFadeActive = pageNotifyActive_ &&
        !pageNotifyCompositorDriven_ &&
        (pageElapsed < kPageNotifyFadeMs ||
         pageElapsed >= kPageNotifyVisibleMs -
             kPageNotifyFadeMs);
    const bool keep =
        (popupAnimation_.IsAnimating() &&
            !popupAnimationCompositorDriven_) ||
        (luaWidgetPanelAnimation_.IsAnimating() &&
            !luaWidgetPanelAnimationCompositorDriven_) ||
        quickNavigationAnimation_.IsAnimating() ||
        !dockLaunchBounces_.empty() ||
        pageFadeActive ||
        desktopPointerPresentPending_ ||
        floatingDockPointerPresentPending_;
    if (!keep)
        uiAnimationFrameToken_ = 0;
    return keep;
}
