#include "app.h"
#include "native_menu_presentation_rules.h"

namespace
{
HRESULT CreateSmoothStepAnimation(
    IDCompositionDesktopDevice* device,
    float from, float to,
    UINT durationMilliseconds,
    IDCompositionAnimation** animation,
    float normalizedStartSlope = 0.0f)
{
    if (!device || !animation || durationMilliseconds == 0)
        return E_INVALIDARG;
    *animation = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> result;
    HRESULT hr = device->CreateAnimation(&result);
    const double duration =
        static_cast<double>(durationMilliseconds) / 1000.0;
    const double delta =
        static_cast<double>(to) - static_cast<double>(from);
    const double startSlope = std::clamp(
        static_cast<double>(normalizedStartSlope), 0.0, 2.0);
    const double startVelocity =
        delta * startSlope / duration;
    if (SUCCEEDED(hr))
    {
        hr = result->AddCubic(
            0.0,
            from,
            static_cast<float>(startVelocity),
            static_cast<float>(
                3.0 * delta / (duration * duration) -
                2.0 * startVelocity / duration),
            static_cast<float>(
                -2.0 * delta /
                    (duration * duration * duration) +
                startVelocity / (duration * duration)));
    }
    if (SUCCEEDED(hr))
        hr = result->End(duration, to);
    if (SUCCEEDED(hr))
        *animation = result.Detach();
    return hr;
}
}

// Independent DComp layers for static popup snapshots. The desktop surface is
// cleared once at animation start; subsequent frames change only visual
// properties and never invoke the popup/Lua renderers.

bool DesktopApp::PrepareCompositionAnimationOverlay(
    UiCompositionAnimationOverlay& overlay,
    const DragRenderCache& cache,
    const RECT& bounds,
    UiCompositionAnimationHost host)
{
    ResetCompositionAnimationOverlay(overlay);
    if (!dcompDevice_ || IsRectEmpty(&bounds))
        return false;

    IDCompositionVisual2* parentVisual = nullptr;
    if (host == UiCompositionAnimationHost::FloatingPopup)
    {
        if (!floatingPopupHwnd_ ||
            !IsWindow(floatingPopupHwnd_) ||
            IsRectEmpty(&floatingPopupWindowBounds_) ||
            FAILED(CreateOrResizeFloatingPopupCompositionSurface()))
        {
            return false;
        }
        parentVisual = floatingPopupDcompVisual_.Get();
    }
    else
    {
        parentVisual = dcompVisual_.Get();
    }
    if (!parentVisual)
        return false;

    if (overlay.visual && overlay.host != host)
    {
        IDCompositionVisual2* previousParent =
            overlay.host == UiCompositionAnimationHost::FloatingPopup
                ? floatingPopupDcompVisual_.Get()
                : dcompVisual_.Get();
        if (previousParent)
            (void)previousParent->RemoveVisual(overlay.visual.Get());
        overlay.visual.Reset();
        overlay.effect.Reset();
        overlay.scaleTransform.Reset();
    }

    const UINT width = static_cast<UINT>(
        std::max<LONG>(1, bounds.right - bounds.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(1, bounds.bottom - bounds.top));
    if (!overlay.visual)
    {
        HRESULT hr = dcompDevice_->CreateVisual(&overlay.visual);
        if (FAILED(hr) || !overlay.visual)
            return false;
        hr = dcompDevice_->CreateEffectGroup(&overlay.effect);
        if (FAILED(hr) || !overlay.effect)
        {
            overlay.visual.Reset();
            return false;
        }
        hr = dcompDevice_->CreateScaleTransform(
            &overlay.scaleTransform);
        if (FAILED(hr) || !overlay.scaleTransform)
        {
            overlay.effect.Reset();
            overlay.visual.Reset();
            return false;
        }
        overlay.effect->SetOpacity(0.0f);
        overlay.visual->SetEffect(overlay.effect.Get());
        overlay.visual->SetTransform(
            overlay.scaleTransform.Get());
        overlay.host = host;
        hr = parentVisual->AddVisual(
            overlay.visual.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr) &&
            host == UiCompositionAnimationHost::Desktop)
            hr = SyncDesktopCompositionRootZOrder();
        else if (SUCCEEDED(hr))
            hr = SyncFloatingPopupCompositionRootZOrder();
        if (FAILED(hr))
        {
            (void)parentVisual->RemoveVisual(
                overlay.visual.Get());
            overlay.effect.Reset();
            overlay.scaleTransform.Reset();
            overlay.visual.Reset();
            return false;
        }
    }

    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &overlay.surface);
    if (FAILED(hr) || !overlay.surface)
        return false;

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = overlay.surface->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        overlay.surface.Reset();
        return false;
    }

    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->Clear(D2D1::ColorF(0, 0, 0, 0));
    const bool drawn = cache.DrawAt(
        context.Get(),
        D2D1::Point2F(
            static_cast<float>(updateOffset.x),
            static_cast<float>(updateOffset.y)),
        D2D1_INTERPOLATION_MODE_LINEAR);
    context.Reset();
    const HRESULT endDrawHr = overlay.surface->EndDraw();
    if (!drawn || FAILED(endDrawHr))
    {
        overlay.surface.Reset();
        return false;
    }

    overlay.bounds = bounds;
    overlay.visual->SetContent(overlay.surface.Get());
    POINT visualOffset{ bounds.left, bounds.top };
    if (host == UiCompositionAnimationHost::FloatingPopup)
    {
        visualOffset = snowdesktop::floating_popup_rules::
            AnimationVisualOffset(
                bounds, floatingPopupWindowBounds_);
    }
    overlay.visual->SetOffsetX(
        static_cast<float>(visualOffset.x));
    overlay.visual->SetOffsetY(
        static_cast<float>(visualOffset.y));
    overlay.scaleTransform->SetScaleX(1.0f);
    overlay.scaleTransform->SetScaleY(1.0f);
    overlay.scaleTransform->SetCenterX(0.0f);
    overlay.scaleTransform->SetCenterY(0.0f);
    overlay.effect->SetOpacity(0.0f);
    overlay.active = true;
    return true;
}

bool DesktopApp::UpdateCompositionAnimationOverlay(
    UiCompositionAnimationOverlay& overlay,
    float scale, POINT anchor, float opacity,
    bool commit)
{
    if (!overlay.active || !overlay.visual ||
        !overlay.effect || !overlay.scaleTransform ||
        !dcompDevice_)
        return false;

    const D2D1_POINT_2F localAnchor = D2D1::Point2F(
        static_cast<float>(anchor.x - overlay.bounds.left),
        static_cast<float>(anchor.y - overlay.bounds.top));
    HRESULT hr = overlay.scaleTransform->SetCenterX(
        localAnchor.x);
    if (SUCCEEDED(hr))
        hr = overlay.scaleTransform->SetCenterY(localAnchor.y);
    if (SUCCEEDED(hr))
        hr = overlay.scaleTransform->SetScaleX(scale);
    if (SUCCEEDED(hr))
        hr = overlay.scaleTransform->SetScaleY(scale);
    if (SUCCEEDED(hr))
        hr = overlay.effect->SetOpacity(
            std::clamp(opacity, 0.0f, 1.0f));
    if (FAILED(hr))
        return false;

    return !commit || CommitCompositionAnimationFrame();
}

bool DesktopApp::AnimateCompositionAnimationOverlay(
    UiCompositionAnimationOverlay& overlay,
    float fromScale, float toScale,
    POINT anchor,
    float fromOpacity, float toOpacity,
    UINT durationMilliseconds,
    float normalizedScaleStartSlope)
{
    if (!overlay.active || !overlay.visual ||
        !overlay.effect || !overlay.scaleTransform ||
        !dcompDevice_ || durationMilliseconds == 0)
        return false;

    const D2D1_POINT_2F localAnchor = D2D1::Point2F(
        static_cast<float>(anchor.x - overlay.bounds.left),
        static_cast<float>(anchor.y - overlay.bounds.top));
    Microsoft::WRL::ComPtr<IDCompositionAnimation> scaleAnimation;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> opacityAnimation;
    HRESULT hr = CreateSmoothStepAnimation(
        dcompDevice_.Get(), fromScale, toScale,
        durationMilliseconds, &scaleAnimation,
        normalizedScaleStartSlope);
    if (SUCCEEDED(hr))
    {
        hr = CreateSmoothStepAnimation(
            dcompDevice_.Get(), fromOpacity, toOpacity,
            durationMilliseconds, &opacityAnimation);
    }
    if (SUCCEEDED(hr))
        hr = overlay.scaleTransform->SetCenterX(localAnchor.x);
    if (SUCCEEDED(hr))
        hr = overlay.scaleTransform->SetCenterY(localAnchor.y);
    if (SUCCEEDED(hr))
        hr = overlay.scaleTransform->SetScaleX(
            scaleAnimation.Get());
    if (SUCCEEDED(hr))
        hr = overlay.scaleTransform->SetScaleY(
            scaleAnimation.Get());
    if (SUCCEEDED(hr))
        hr = overlay.effect->SetOpacity(opacityAnimation.Get());
    if (FAILED(hr))
        return false;
    return CommitCompositionAnimationFrame();
}

bool DesktopApp::CommitCompositionAnimationFrame()
{
    if (!dcompDevice_)
        return false;
    compositionCommitPending_ = true;
    RecordShellHoverTrace(
        ShellHoverTraceEvent::CommitQueued);
    return true;
}

bool DesktopApp::FlushPendingCompositionCommit()
{
    if (!compositionCommitPending_)
        return true;
    if (!dcompDevice_)
    {
        compositionCommitPending_ = false;
        return false;
    }
    const double commitStart =
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    const HRESULT hr = dcompDevice_->Commit();
    if (SUCCEEDED(hr))
        compositionCommitPending_ = false;
    RecordShellHoverTrace(
        ShellHoverTraceEvent::CommitFlushed);
    uiAnimationScheduler_.RecordCommitDuration(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() -
        commitStart);
    if (SUCCEEDED(hr))
        return true;

    // This device contains the desktop and floating-Dock trees. Recover those
    // surfaces without touching Quick Navigation, which owns a separate DComp
    // device specifically so its animation cannot block pointer presentation.
    compositionCommitPending_ = false;
    RecoverCompositionRenderFailure(
        L"Batched DComp Commit", hr);
    if (floatingDockVisible_)
        RecoverFloatingDockCompositionFailure(
            L"Batched DComp Commit", hr);
    if (ShouldShowFloatingPopupWindow())
        RecoverFloatingPopupCompositionFailure(
            L"Batched DComp Commit", hr);
    EnsureUiAnimationFrame();
    return false;
}

bool DesktopApp::WaitForCompositionPresentation(
    const wchar_t* diagnosticContext)
{
    if (!FlushPendingCompositionCommit())
        return false;

    const double waitStart =
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    const HRESULT commitCompletionHr = dcompDevice_
        ? dcompDevice_->WaitForCommitCompletion()
        : E_UNEXPECTED;
    // WaitForCommitCompletion only guarantees that DirectComposition has
    // processed the commit. A floating-Dock ownership hand-off must not hide
    // its source HWND until DWM has actually presented the replacement. Run
    // the screen fence on every path; it also covers a paired Windows
    // Composition commit that completed immediately before this call.
    const HRESULT presentationHr = DwmFlush();
    const HRESULT completionHr = SUCCEEDED(commitCompletionHr)
        ? presentationHr
        : (SUCCEEDED(presentationHr)
            ? S_OK : commitCompletionHr);
    uiAnimationScheduler_.RecordCommitDuration(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() -
        waitStart);
    if (SUCCEEDED(completionHr))
        return true;

    wchar_t message[192]{};
    wsprintfW(
        message,
        L"%s composition completion FAILED hr=0x%08X",
        diagnosticContext ? diagnosticContext : L"Dock handoff",
        static_cast<unsigned>(completionHr));
    WriteDiagnosticLogEntry(message);
    return false;
}

bool DesktopApp::CommitQuickNavigationCompositionFrame()
{
    if (!quickNavDcompDevice_)
        return false;
    quickNavCompositionCommitPending_ = true;
    return true;
}

bool DesktopApp::FlushPendingQuickNavigationCompositionCommit()
{
    if (!quickNavCompositionCommitPending_)
        return true;
    if (!quickNavDcompDevice_)
    {
        quickNavCompositionCommitPending_ = false;
        return false;
    }

    const double commitStart =
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    const HRESULT hr = quickNavDcompDevice_->Commit();
    quickNavCompositionCommitPending_ = false;
    uiAnimationScheduler_.RecordCommitDuration(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() -
        commitStart);
    if (SUCCEEDED(hr))
        return true;

    RecoverQuickNavCompositionFailure(
        L"Isolated DComp Commit", hr);
    EnsureUiAnimationFrame();
    return false;
}

bool DesktopApp::StartQuickNavigationCompositionAnimation()
{
    if (!quickNavigationAnimation_.IsAnimating())
        return false;

    if (quickNavigationAnimationCompletionToken_)
    {
        uiAnimationScheduler_.Cancel(
            quickNavigationAnimationCompletionToken_);
    }
    quickNavigationAnimationCompletionToken_ = 0;
    if (quickNavigationAnimationFrameToken_)
    {
        uiAnimationScheduler_.Cancel(
            quickNavigationAnimationFrameToken_);
    }
    quickNavigationAnimationFrameToken_ = 0;
    quickNavigationAnimationCompositorDriven_ = false;

    // Snap both compositor trees to the same logical frame before attaching
    // native animations. This also disconnects a previous animation during a
    // rapid open/close reversal and hides the native search edit until rest.
    ApplyQuickNavigationAnimationFrame();
    if (!quickNavDcompDevice_ || !quickNavDcompVisual_ ||
        !quickNavDcompEffect_ || !quickNavDcompScaleTransform_)
    {
        return false;
    }

    const auto visual = quickNavigationAnimation_.GetVisual();
    const bool opening = quickNavigationAnimation_.IsOpening();
    const float targetScale = opening
        ? 1.0f
        : snowdesktop::quick_navigation_animation_rules::
            kMinimumScale;
    const float targetOpacity = opening ? 1.0f : 0.0f;
    const float normalizedStartSlope =
        snowdesktop::quick_navigation_animation_rules::
            SegmentNormalizedStartSlope(
                visual.progress, opening);
    const float remaining = opening
        ? 1.0f - visual.progress : visual.progress;
    const UINT duration = std::max<UINT>(
        1, static_cast<UINT>(std::lround(
            remaining * static_cast<float>(opening
                ? snowdesktop::quick_navigation_animation_rules::
                    kOpenDurationMs
                : snowdesktop::quick_navigation_animation_rules::
                    kCloseDurationMs))));

    bool backdropAnimationStarted = false;
    if (quickNavGlassTheme_ &&
        quickNavBackdropCompositor_.IsAvailable())
    {
        backdropAnimationStarted =
            quickNavBackdropCompositor_.
                StartVisualTransformAnimation(
                    visual.scale, targetScale,
                    visual.opacity, targetOpacity,
                    static_cast<float>(
                        quickNavigationAnimationAnchorPoint_.x -
                        quickNavigationHostRect_.left),
                    static_cast<float>(
                        quickNavigationAnimationAnchorPoint_.y -
                        quickNavigationHostRect_.top),
                    duration, normalizedStartSlope);
        if (!backdropAnimationStarted)
            return false;
    }

    Microsoft::WRL::ComPtr<IDCompositionAnimation> scaleAnimation;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> opacityAnimation;
    HRESULT hr = CreateSmoothStepAnimation(
        quickNavDcompDevice_.Get(), visual.scale, targetScale,
        duration, &scaleAnimation, normalizedStartSlope);
    if (SUCCEEDED(hr))
    {
        hr = CreateSmoothStepAnimation(
            quickNavDcompDevice_.Get(), visual.opacity, targetOpacity,
            duration, &opacityAnimation, normalizedStartSlope);
    }
    if (SUCCEEDED(hr))
    {
        hr = quickNavDcompScaleTransform_->SetScaleX(
            scaleAnimation.Get());
    }
    if (SUCCEEDED(hr))
    {
        hr = quickNavDcompScaleTransform_->SetScaleY(
            scaleAnimation.Get());
    }
    if (SUCCEEDED(hr))
        hr = quickNavDcompEffect_->SetOpacity(opacityAnimation.Get());
    if (FAILED(hr) || !CommitQuickNavigationCompositionFrame())
    {
        ApplyQuickNavigationAnimationFrame();
        return false;
    }

    quickNavigationAnimationCompositorDriven_ = true;
    quickNavigationAnimationCompletionToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            duration + 2,
            [this](snowdesktop::UiScheduleToken token) {
                if (quickNavigationAnimationCompletionToken_ != token)
                    return;
                quickNavigationAnimationCompletionToken_ = 0;
                quickNavigationAnimationCompositorDriven_ = false;
                quickNavigationAnimation_.Advance(
                    static_cast<std::uint64_t>(
                        snowdesktop::UiAnimationScheduler::
                            MonotonicMilliseconds()));
                if (!quickNavigationAnimation_.IsAnimating() &&
                    quickNavigationAnimation_.IsHidden())
                {
                    FinalizeCloseQuickNavigation();
                    return;
                }
                if (quickNavigationAnimation_.IsAnimating())
                {
                    if (StartQuickNavigationCompositionAnimation())
                        return;
                    ApplyQuickNavigationAnimationFrame();
                    EnsureUiAnimationFrame();
                    return;
                }
                ApplyQuickNavigationAnimationFrame();
            });
    if (!quickNavigationAnimationCompletionToken_)
    {
        quickNavigationAnimationCompositorDriven_ = false;
        ApplyQuickNavigationAnimationFrame();
        return false;
    }
    return true;
}

void DesktopApp::FlushNativeMenuPresentation()
{
    if (!snowdesktop::native_menu_presentation_rules::
            ShouldFlushAfterOwnerMessage(
                shellPopupMenuLayerDepth_ > 0,
                compositionPaintInProgress_,
                quickNavCompositionPaintInProgress_,
                floatingDockCompositionPaintInProgress_))
        return;

    FlushPendingCompositionCommit();
    FlushPendingQuickNavigationCompositionCommit();
}

void DesktopApp::ClearDesktopBehindCompositionAnimation(
    const RECT& bounds)
{
    if (!hwnd_ || !IsWindow(hwnd_) ||
        compositionPaintInProgress_ || IsRectEmpty(&bounds))
        return;
    InvalidateRect(hwnd_, &bounds, FALSE);
    RECT update{};
    if (!GetUpdateRect(hwnd_, &update, FALSE))
        return;
    ValidateRect(hwnd_, &update);
    OnPaint(&update);
}

void DesktopApp::PrepareCompositionAnimationOverlayRetirement(
    UiCompositionAnimationOverlay& overlay,
    const RECT& bounds)
{
    if (!overlay.active)
        return;

    overlay.active = false;
    if (overlay.host ==
        UiCompositionAnimationHost::FloatingPopup)
    {
        // Paint the final live popup surface while the snapshot child visual
        // is still attached. Its caller removes the snapshot in the same
        // pending DComp transaction, so the shared topmost host never exposes
        // an empty frame between the two content owners.
        if (!RenderFloatingPopupCompositionFrame() &&
            floatingPopupHwnd_ &&
            IsWindow(floatingPopupHwnd_))
        {
            InvalidateRect(
                floatingPopupHwnd_, nullptr, FALSE);
        }
        return;
    }

    // The normal desktop renderer skips popup content while its snapshot
    // overlay is active. Release only that logical suppression first, then
    // paint the final static frame while the snapshot is still attached to
    // the DComp tree. ResetCompositionAnimationOverlay can subsequently
    // retire the snapshot in the same pending DComp transaction, so DWM
    // never observes an empty frame between the two content owners.
    ClearDesktopBehindCompositionAnimation(bounds);
}

void DesktopApp::ResetCompositionAnimationOverlay(
    UiCompositionAnimationOverlay& overlay)
{
    if (overlay.effect)
        overlay.effect->SetOpacity(0.0f);
    if (overlay.visual)
    {
        overlay.visual->SetContent(nullptr);
    }
    if (overlay.scaleTransform)
    {
        overlay.scaleTransform->SetScaleX(1.0f);
        overlay.scaleTransform->SetScaleY(1.0f);
        overlay.scaleTransform->SetCenterX(0.0f);
        overlay.scaleTransform->SetCenterY(0.0f);
    }
    overlay.surface.Reset();
    overlay.bounds = {};
    overlay.active = false;
}

bool DesktopApp::UpdateCollectionPopupCompositionAnimation(
    bool commit)
{
    ApplyCollectionPopupBackdropAnimationFrame();
    if (!popupAnimationOverlay_.active)
        return false;
    const auto visual = popupAnimation_.GetVisual();
    POINT anchor{
        (popupRect_.left + popupRect_.right) / 2,
        (popupRect_.top + popupRect_.bottom) / 2,
    };
    if (popupHasAnchor_)
    {
        anchor.x = std::clamp(
            popupAnchorPoint_.x, popupRect_.left, popupRect_.right);
        anchor.y = std::clamp(
            popupAnchorPoint_.y, popupRect_.top, popupRect_.bottom);
    }
    return UpdateCompositionAnimationOverlay(
        popupAnimationOverlay_, visual.scale,
        anchor, visual.visible ? 1.0f : 0.0f,
        commit);
}

bool DesktopApp::UpdateLuaWidgetPanelCompositionAnimation(
    bool commit)
{
    if (!luaWidgetPanelAnimationOverlay_.active)
        return false;
    const auto visual = luaWidgetPanelAnimation_.GetVisual();
    const RECT panel = GetLuaWidgetPanelRect();
    POINT anchor = luaWidgetPanelRequest_.surface == "dialog"
        ? POINT{ (panel.left + panel.right) / 2,
            (panel.top + panel.bottom) / 2 }
        : POINT{
            std::clamp(luaWidgetPanelAnchorPoint_.x,
                panel.left, panel.right),
            std::clamp(luaWidgetPanelAnchorPoint_.y,
                panel.top, panel.bottom),
        };
    return UpdateCompositionAnimationOverlay(
        luaWidgetPanelAnimationOverlay_, visual.scale,
        anchor, visual.visible ? 1.0f : 0.0f,
        commit);
}

bool DesktopApp::StartCollectionPopupCompositionAnimation()
{
    if (!popupAnimationOverlay_.active ||
        !popupAnimation_.IsAnimating())
        return false;
    // The popup snapshot is a DirectComposition visual while its native
    // glass lives in a Windows Composition target. Independent compositor
    // animations can begin on different presented frames even when they use
    // the same duration and easing. Keep glass popups on the display-cadence
    // scheduler so both trees receive the same sampled scale before their
    // commits are submitted.
    if (collectionPopupGlassTheme_ &&
        collectionPopupBackdropCompositor_.IsAvailable())
        return false;
    if (popupAnimationCompletionToken_)
        uiAnimationScheduler_.Cancel(
            popupAnimationCompletionToken_);
    popupAnimationCompletionToken_ = 0;
    if (popupAnimationFrameToken_)
        uiAnimationScheduler_.Cancel(
            popupAnimationFrameToken_);
    popupAnimationFrameToken_ = 0;

    const auto visual = popupAnimation_.GetVisual();
    const bool opening = popupAnimation_.IsInteractive();
    const float normalizedScaleStartSlope =
        snowdesktop::popup_animation_rules::
            ScaleSegmentNormalizedStartSlope(
                visual.progress, opening);
    const float remaining = opening
        ? 1.0f - visual.progress : visual.progress;
    const UINT duration = std::max<UINT>(
        1, static_cast<UINT>(std::lround(
            remaining * static_cast<float>(opening
                ? snowdesktop::popup_animation_rules::kOpenDurationMs
                : snowdesktop::popup_animation_rules::kCloseDurationMs))));
    POINT anchor{
        (popupRect_.left + popupRect_.right) / 2,
        (popupRect_.top + popupRect_.bottom) / 2,
    };
    if (popupHasAnchor_)
    {
        anchor.x = std::clamp(
            popupAnchorPoint_.x, popupRect_.left, popupRect_.right);
        anchor.y = std::clamp(
            popupAnchorPoint_.y, popupRect_.top, popupRect_.bottom);
    }
    if (!AnimateCompositionAnimationOverlay(
            popupAnimationOverlay_,
            visual.scale,
            opening ? 1.0f :
                snowdesktop::popup_animation_rules::kMinimumScale,
            anchor, 1.0f, 1.0f, duration,
            normalizedScaleStartSlope))
        return false;

    popupAnimationCompositorDriven_ = true;
    popupAnimationCompletionToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            duration + 2,
            [this](snowdesktop::UiScheduleToken token) {
                if (popupAnimationCompletionToken_ != token)
                    return;
                popupAnimationCompletionToken_ = 0;
                popupAnimationCompositorDriven_ = false;
                popupAnimation_.Advance(static_cast<std::uint64_t>(
                    snowdesktop::UiAnimationScheduler::
                        MonotonicMilliseconds()));
                // A hidden collection popup is retired by its own finalizer.
                // Do not hide the backdrop helper here first: the finalizer
                // closes it together with the shared popup host when no other
                // popup content remains.
                if (!popupAnimation_.IsAnimating() &&
                    popupAnimation_.IsHidden())
                {
                    FinalizeCloseCollectionPopup();
                    return;
                }
                ApplyCollectionPopupBackdropAnimationFrame();
                if (popupAnimation_.IsAnimating())
                {
                    if (StartCollectionPopupCompositionAnimation())
                        return;
                    EnsureUiAnimationFrame();
                    return;
                }
                const RECT dirty = popupAnimationCacheRect_;
                PrepareCompositionAnimationOverlayRetirement(
                    popupAnimationOverlay_, dirty);
                ResetCollectionPopupAnimationCache();
                if (hwnd_ && IsWindow(hwnd_))
                    InvalidateRect(hwnd_, &dirty, FALSE);
            });
    if (!popupAnimationCompletionToken_)
    {
        popupAnimationCompositorDriven_ = false;
        ApplyCollectionPopupBackdropAnimationFrame();
        return false;
    }
    return true;
}

bool DesktopApp::StartLuaWidgetPanelCompositionAnimation()
{
    if (!luaWidgetPanelAnimationOverlay_.active ||
        !luaWidgetPanelAnimation_.IsAnimating())
        return false;
    if (luaWidgetPanelAnimationCompletionToken_)
    {
        uiAnimationScheduler_.Cancel(
            luaWidgetPanelAnimationCompletionToken_);
    }
    luaWidgetPanelAnimationCompletionToken_ = 0;
    if (luaPanelAnimationFrameToken_)
        uiAnimationScheduler_.Cancel(
            luaPanelAnimationFrameToken_);
    luaPanelAnimationFrameToken_ = 0;

    const auto visual = luaWidgetPanelAnimation_.GetVisual();
    const bool opening =
        luaWidgetPanelAnimation_.IsInteractive();
    const float normalizedScaleStartSlope =
        snowdesktop::popup_animation_rules::
            ScaleSegmentNormalizedStartSlope(
                visual.progress, opening);
    const float remaining = opening
        ? 1.0f - visual.progress : visual.progress;
    const UINT duration = std::max<UINT>(
        1, static_cast<UINT>(std::lround(
            remaining * static_cast<float>(opening
                ? snowdesktop::popup_animation_rules::kOpenDurationMs
                : snowdesktop::popup_animation_rules::kCloseDurationMs))));
    const RECT panel = GetLuaWidgetPanelRect();
    const POINT anchor = luaWidgetPanelRequest_.surface == "dialog"
        ? POINT{ (panel.left + panel.right) / 2,
            (panel.top + panel.bottom) / 2 }
        : POINT{
            std::clamp(luaWidgetPanelAnchorPoint_.x,
                panel.left, panel.right),
            std::clamp(luaWidgetPanelAnchorPoint_.y,
                panel.top, panel.bottom),
        };
    if (!AnimateCompositionAnimationOverlay(
            luaWidgetPanelAnimationOverlay_,
            visual.scale,
            opening ? 1.0f :
                snowdesktop::popup_animation_rules::kMinimumScale,
            anchor, 1.0f, 1.0f, duration,
            normalizedScaleStartSlope))
        return false;

    luaWidgetPanelAnimationCompositorDriven_ = true;
    luaWidgetPanelAnimationCompletionToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            duration + 2,
            [this](snowdesktop::UiScheduleToken token) {
                if (luaWidgetPanelAnimationCompletionToken_ != token)
                    return;
                luaWidgetPanelAnimationCompletionToken_ = 0;
                luaWidgetPanelAnimationCompositorDriven_ = false;
                luaWidgetPanelAnimation_.Advance(
                    static_cast<std::uint64_t>(
                        snowdesktop::UiAnimationScheduler::
                            MonotonicMilliseconds()));
                if (luaWidgetPanelAnimation_.IsAnimating())
                {
                    if (StartLuaWidgetPanelCompositionAnimation())
                        return;
                    EnsureUiAnimationFrame();
                    return;
                }
                if (luaWidgetPanelAnimation_.IsHidden())
                {
                    FinalizeCloseLuaWidgetPanel();
                    return;
                }
                const RECT dirty =
                    luaWidgetPanelAnimationCacheRect_;
                PrepareCompositionAnimationOverlayRetirement(
                    luaWidgetPanelAnimationOverlay_, dirty);
                ResetLuaWidgetPanelAnimationCache();
                if (hwnd_ && IsWindow(hwnd_))
                    InvalidateRect(hwnd_, &dirty, FALSE);
            });
    if (!luaWidgetPanelAnimationCompletionToken_)
    {
        luaWidgetPanelAnimationCompositorDriven_ = false;
        return false;
    }
    return true;
}
