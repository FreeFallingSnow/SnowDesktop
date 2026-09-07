#include "app.h"

namespace
{
std::uint64_t AnimationTick(double nowMilliseconds)
{
    return static_cast<std::uint64_t>(
        std::max(0.0, std::floor(nowMilliseconds)));
}
}

void DesktopApp::ApplyAnimationPreferences(bool systemChanged)
{
    namespace motion = snowdesktop::animation;
    NormalizeGeneralAnimationSettings(generalSettings_);
    NormalizeDockSettings(dockSettings_);
    if (systemChanged) (void)motion::SystemAnimationsEnabled(true);
    const bool transitionChanged = systemChanged ||
        motion::detail::mode.load() != generalSettings_.animationMode ||
        motion::detail::popupEffect.load() != generalSettings_.popupAnimationEffect ||
        motion::detail::speed.load() != generalSettings_.animationSpeed;
    const bool windowChanged = transitionChanged ||
        motion::RuntimeWindowEffect() != dockSettings_.windowEffect;
    const bool pageNotifyPolicyChanged =
        motion::detail::mode.load() != generalSettings_.animationMode ||
        motion::detail::popupEffect.load() != generalSettings_.popupAnimationEffect ||
        motion::detail::speed.load() != generalSettings_.animationSpeed;
    motion::SetRuntimePreferences(generalSettings_.animationMode,
        generalSettings_.popupAnimationEffect, generalSettings_.animationSpeed,
        generalSettings_.animationFrameLimit, generalSettings_.animationEnergySaver,
        generalSettings_.animationOnBattery, dockSettings_.windowEffect,
        static_cast<int>(dockSettings_.position));

    const bool fade = generalSettings_.popupAnimationEffect == motion::Fade;
    const double durationScale = motion::RuntimeDurationScale();
    popupAnimation_.Configure(fade, durationScale);
    luaWidgetPanelAnimation_.Configure(fade, durationScale);
    quickNavigationAnimation_.Configure(fade, durationScale);

    // Finish the old timeline when its duration/effect changes.
    // Closing finalizers also preserve queued actions and pending popup opens.
    if (transitionChanged)
    {
        if (popupAnimation_.IsAnimating())
        {
            if (popupAnimation_.IsClosing()) FinalizeCloseCollectionPopup();
            else
            {
                popupAnimation_.ShowImmediately();
                ResetCollectionPopupAnimationCache();
                ApplyCollectionPopupBackdropAnimationFrame();
            }
        }
        if (luaWidgetPanelAnimation_.IsAnimating())
        {
            if (luaWidgetPanelAnimation_.IsClosing()) FinalizeCloseLuaWidgetPanel();
            else
            {
                luaWidgetPanelAnimation_.ShowImmediately();
                ResetLuaWidgetPanelAnimationCache();
            }
        }
        if (quickNavigationAnimation_.IsAnimating())
        {
            if (quickNavigationAnimationCompletionToken_)
                uiAnimationScheduler_.Cancel(quickNavigationAnimationCompletionToken_);
            quickNavigationAnimationCompletionToken_ = 0;
            quickNavigationAnimationCompositorDriven_ = false;
            if (quickNavigationAnimation_.IsClosing()) FinalizeCloseQuickNavigation();
            else
            {
                quickNavigationAnimation_.ShowImmediately();
                ApplyQuickNavigationAnimationFrame();
            }
        }
    }
    if (widgetEngine_)
        widgetEngine_->ApplyHostAnimationPreferences();
    if (windowChanged && dockWindowTransition_)
        dockWindowTransition_->CompleteImmediately();
    if (!motion::RuntimeAnimationsEnabled() || dockSettings_.launchEffect == 0)
    {
        dockLaunchBounces_.clear();
        if (dockBounceAnimationFrameToken_)
            uiAnimationScheduler_.Cancel(dockBounceAnimationFrameToken_);
        dockBounceAnimationFrameToken_ = 0;
    }
    if (pageNotifyActive_ &&
        (pageNotifyPolicyChanged || !motion::RuntimeAnimationsEnabled()))
    {
        if (pageNotifyFadeOutToken_)
            uiAnimationScheduler_.Cancel(pageNotifyFadeOutToken_);
        pageNotifyFadeOutToken_ = 0;
        if (pageNotifyAnimationFrameToken_)
            uiAnimationScheduler_.Cancel(pageNotifyAnimationFrameToken_);
        pageNotifyAnimationFrameToken_ = 0;
        pageNotifyActive_ = false;
        pageNotifyUseAnimation_ = false;
        pageNotifyText_.clear();
        ResetPageNotifyTextCache();
    }
    InvalidateDockContainers();
    if (hwnd_ && IsWindow(hwnd_))
    {
        InvalidateDragStaticScene();
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateFloatingPopupWindowBounds(true);
        InvalidateFloatingDockWindow(false);
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
    const DWORD pageVisibleMs = kPageNotifyVisibleMs -
        2 * kPageNotifyFadeMs + 2 * pageNotifyFadeMs_;
    const bool pageFadeActive = pageNotifyActive_ &&
        pageNotifyUseAnimation_ &&
        snowdesktop::animation::RuntimeAnimationsEnabled() &&
        !pageNotifyCompositorDriven_ &&
        (pageElapsed < pageNotifyFadeMs_ ||
         pageElapsed >= pageVisibleMs - pageNotifyFadeMs_);
    if (!pageNotifyAnimationFrameToken_ && pageFadeActive)
    {
        pageNotifyAnimationFrameToken_ =
            uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::Desktop,
                [this](double) {
                    if (!pageNotifyActive_ ||
                        !pageNotifyUseAnimation_ ||
                        !snowdesktop::animation::RuntimeAnimationsEnabled() ||
                        pageNotifyCompositorDriven_)
                    {
                        pageNotifyAnimationFrameToken_ = 0;
                        return false;
                    }

                    const DWORD elapsed =
                        GetTickCount() - pageNotifyStartTick_;
                    const DWORD visibleMs = kPageNotifyVisibleMs -
                        2 * kPageNotifyFadeMs + 2 * pageNotifyFadeMs_;
                    const RECT dirty = GetPageNotifyBounds();
                    if (elapsed >= visibleMs)
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
                        if (elapsed < pageNotifyFadeMs_)
                        {
                            opacity = static_cast<float>(elapsed) /
                                static_cast<float>(pageNotifyFadeMs_);
                        }
                        else if (elapsed >=
                            visibleMs - pageNotifyFadeMs_)
                        {
                            opacity = static_cast<float>(
                                visibleMs - elapsed) /
                                static_cast<float>(pageNotifyFadeMs_);
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
                        pageNotifyUseAnimation_ &&
                        snowdesktop::animation::RuntimeAnimationsEnabled() &&
                        !pageNotifyCompositorDriven_ &&
                        (currentElapsed < pageNotifyFadeMs_ ||
                         currentElapsed >= visibleMs - pageNotifyFadeMs_);
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
                        floatingDockHost_ &&
                        !floatingDockHost_->compositionPaintInProgress)
                    {
                        floatingDockPointerPresentPending_ = false;
                        RECT update{};
                        if (GetUpdateRect(
                                floatingDockHwnd_, &update, FALSE))
                        {
                            ValidateRect(floatingDockHwnd_, &update);
                            if (!RenderFloatingDockCompositionFrame(
                                    *floatingDockHost_))
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
