#include "app/app.h"
#include "ui/render/composition_motion_curve.h"
#include "navigation/quick_navigation_animation_rules.h"
#include "app/navigation/quick_navigation_state.h"
#include "diagnostics/diagnostic_log.h"
#include "app/dock/persistent_dock_host.h"
#include "app/lifecycle/startup_diagnostics.h"
#include "diagnostics/performance_trace.h"
#include "app/menus/native_menu_presentation_rules.h"
#include "widget/view/widget_composition_layer_rules.h"

#include <utility>

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

bool DesktopApp::SubmitDesktopWidgetFadeOut(DesktopWidgetCompositionItem& item)
{
    if (!dcompDevice_ || !item.visual) return false;
    auto motion = item.hoverFade->motion;
    motion.Advance(static_cast<std::uint64_t>(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
    const auto sample = motion.GetVisual();
    const auto duration = static_cast<UINT>(std::max(1.0, std::round(
        sample.progress * motion.DurationMilliseconds(false))));
    // Remaining segment of smoothstep(p), played from p down to zero. Both
    // compositors use this slope when the pointer reverses an opening fade.
    const float slope = 6.f * (1.f - sample.progress) / (3.f - 2.f * sample.progress);
    ComPtr<IDCompositionAnimation> opacity;
    HRESULT hr = CreateSmoothStepAnimation(dcompDevice_.Get(), sample.opacity, 0.f, duration, &opacity, slope);
    if (SUCCEEDED(hr) && !item.opacityEffect)
    {
        ComPtr<IDCompositionEffectGroup> effect;
        hr = dcompDevice_->CreateEffectGroup(&effect);
        if (SUCCEEDED(hr)) hr = item.visual->SetEffect(effect.Get());
        if (SUCCEEDED(hr)) item.opacityEffect = std::move(effect);
    }
    if (SUCCEEDED(hr)) hr = item.opacityEffect->SetOpacity(opacity.Get());
    if (FAILED(hr)) return false;
    // The next ordinary opacity write must replace the native track, even
    // when re-entering the widget at the same previously submitted value.
    item.submittedOpacity = -1.f;
    return desktopBackdropCompositor_.StartOwnerFadeOut(item.BackdropOwnerKey(), sample.opacity, duration, slope) &&
        item.iconBackdrops.StartFadeOut(desktopBackdropCompositor_, sample.opacity, duration, slope);
}

bool DesktopApp::StartDesktopWidgetHoverRoll(DesktopWidgetCompositionItem& item,
    const RECT& frame, LONG collapsedHeight, float radius,
    const snowdesktop::popup_motion_curve::Timeline& timeline)
{
    constexpr float inset =
        snowdesktop::widget_composition_layer_rules::kWidgetSurfaceBorderOverdraw;
    const auto width = static_cast<float>(frame.right - frame.left);
    const auto closed = static_cast<float>(collapsedHeight);
    const auto travel = static_cast<float>(frame.bottom - frame.top) - closed;
    if (travel <= 0.0f || !item.backdropRegistered) return false;
    radius = std::min({radius, width * 0.5f, closed * 0.5f});

    ComPtr<IDCompositionAnimation> bottom;
    HRESULT hr = snowdesktop::CreateCompositionMotionAnimation(dcompDevice_.Get(), timeline,
        &bottom, inset + closed, travel);
    ComPtr<IDCompositionRectangleClip> clip;
    if (SUCCEEDED(hr)) hr = dcompDevice_->CreateRectangleClip(&clip);
    if (SUCCEEDED(hr)) hr = clip->SetLeft(inset);
    if (SUCCEEDED(hr)) hr = clip->SetTop(inset);
    if (SUCCEEDED(hr)) hr = clip->SetRight(inset + width);
    if (SUCCEEDED(hr)) hr = clip->SetTopLeftRadiusX(radius);
    if (SUCCEEDED(hr)) hr = clip->SetTopLeftRadiusY(radius);
    if (SUCCEEDED(hr)) hr = clip->SetTopRightRadiusX(radius);
    if (SUCCEEDED(hr)) hr = clip->SetTopRightRadiusY(radius);
    if (SUCCEEDED(hr)) hr = clip->SetBottomLeftRadiusX(radius);
    if (SUCCEEDED(hr)) hr = clip->SetBottomLeftRadiusY(radius);
    if (SUCCEEDED(hr)) hr = clip->SetBottomRightRadiusX(radius);
    if (SUCCEEDED(hr)) hr = clip->SetBottomRightRadiusY(radius);
    if (SUCCEEDED(hr)) hr = clip->SetBottom(bottom.Get());
    if (SUCCEEDED(hr)) hr = item.visual->SetClip(clip.Get());
    return SUCCEEDED(hr) && desktopBackdropCompositor_.StartPanelHeightAnimation(
        item.BackdropOwnerKey(), closed, radius, timeline);
}

// Independent DComp layers for popup snapshots. Content arrivals update only
// surface pixels; the compositor advances scale/opacity without UI frame work.

DesktopBackdropCompositor& DesktopApp::AnimationHighlightCompositor(const UiCompositionAnimationOverlay& overlay)
{
    return overlay.host == UiCompositionAnimationHost::FloatingPopup
        ? collectionPopupBackdropCompositor_ : desktopBackdropCompositor_;
}

bool DesktopApp::UpdateCompositionAnimationOverlayContent(
    UiCompositionAnimationOverlay& overlay,
    const DragRenderCache& cache)
{
    if (!overlay.surface)
        return false;
    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    const HRESULT hr = overlay.surface->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext), &updateOffset);
    if (FAILED(hr))
        return false;
    if (!rawContext)
    {
        (void)overlay.surface->EndDraw();
        return false;
    }
    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    auto& material = AnimationHighlightCompositor(overlay);
    const auto materialKey = reinterpret_cast<std::uintptr_t>(&overlay);
    const HWND owner = overlay.host == UiCompositionAnimationHost::FloatingPopup ? floatingPopupHwnd_ : hwnd_;
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->Clear(D2D1::ColorF(0, 0, 0, 0));
    DesktopBackdropCompositor::SrgbSceneRecording scene(context.Get());
    const bool drawn = cache.DrawAt(context.Get(),
        D2D1::Point2F(static_cast<float>(updateOffset.x), static_cast<float>(updateOffset.y)),
        D2D1_INTERPOLATION_MODE_LINEAR);
    const auto recorded = scene.Finish();
    const bool prepared = drawn && SUCCEEDED(recorded) && material.PublishSrgbScene(
        owner, materialKey, overlay.bounds, context.Get(), updateOffset, scene.Scene(), overlay.active ? -1.f : 0.f);
    context.Reset();
    const HRESULT endDrawHr = overlay.surface->EndDraw();
    if (!prepared || FAILED(endDrawHr)) return false;
    overlay.presentingHighlights = true;
    return SUCCEEDED(material.SetDirectContentForSrgb(materialKey,
        true, dcompDevice_.Get(), overlay.visual.Get(), overlay.surface.Get()));
}

HRESULT UiCompositionAnimationOverlay::SetPlacementOffset(POINT offset) const
{
    if (!placementTransform)
        return E_UNEXPECTED;
    const HRESULT hr = placementTransform->SetOffsetX(static_cast<float>(offset.x));
    return SUCCEEDED(hr) ? placementTransform->SetOffsetY(static_cast<float>(offset.y)) : hr;
}

bool DesktopApp::PrepareCompositionAnimationOverlay(
    UiCompositionAnimationOverlay& overlay,
    const DragRenderCache& cache,
    const RECT& bounds,
    UiCompositionAnimationHost host)
{
    overlay.FinishSrgbRetirement(AnimationHighlightCompositor(overlay), overlay.host);
    ResetCompositionAnimationOverlay(overlay);
    if (!dcompDevice_ || IsRectEmpty(&bounds))
        return false;

    IDCompositionVisual2* parentVisual = nullptr;
    if (host == UiCompositionAnimationHost::FloatingPopup)
    {
        if (!floatingPopupHwnd_ ||
            !IsWindow(floatingPopupHwnd_) ||
            IsRectEmpty(&floatingPopupContentBounds_) ||
            floatingPopupCompositionPaintInProgress_ ||
            FAILED(EnsureFloatingPopupCompositionRoot()))
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
        AnimationHighlightCompositor(overlay).RetireSrgbAfterDirectCommit(
            reinterpret_cast<std::uintptr_t>(&overlay));
        IDCompositionVisual2* previousParent =
            overlay.host == UiCompositionAnimationHost::FloatingPopup
                ? floatingPopupDcompVisual_.Get()
                : dcompVisual_.Get();
        if (previousParent)
            (void)previousParent->RemoveVisual(overlay.visual.Get());
        overlay.visual.Reset();
        overlay.effect.Reset();
        overlay.scaleTransform.Reset();
        overlay.placementTransform.Reset();
    }

    const UINT width = static_cast<UINT>(
        std::max<LONG>(1, bounds.right - bounds.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(1, bounds.bottom - bounds.top));
    if (!overlay.visual)
    {
        HRESULT hr = dcompDevice_->CreateVisual(&overlay.visual);
        if (SUCCEEDED(hr)) hr = dcompDevice_->CreateEffectGroup(&overlay.effect);
        if (SUCCEEDED(hr)) hr = dcompDevice_->CreateScaleTransform(&overlay.scaleTransform);
        if (SUCCEEDED(hr)) hr = dcompDevice_->CreateTranslateTransform(&overlay.placementTransform);
        // Scale around the snapshot's local anchor, then place it in the
        // host. Visual Offset would be applied before the scale instead.
        IDCompositionTransform* transforms[]{overlay.scaleTransform.Get(), overlay.placementTransform.Get()};
        ComPtr<IDCompositionTransform> transformGroup;
        if (SUCCEEDED(hr)) hr = dcompDevice_->CreateTransformGroup(transforms, 2, &transformGroup);
        if (SUCCEEDED(hr)) hr = overlay.effect->SetOpacity(0.0f);
        if (SUCCEEDED(hr)) hr = overlay.visual->SetEffect(overlay.effect.Get());
        if (SUCCEEDED(hr)) hr = overlay.visual->SetTransform(transformGroup.Get());
        overlay.host = host;
        if (SUCCEEDED(hr)) hr = parentVisual->AddVisual(overlay.visual.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr) &&
            host == UiCompositionAnimationHost::Desktop)
            hr = SyncDesktopCompositionRootZOrder();
        else if (SUCCEEDED(hr))
            hr = SyncFloatingPopupCompositionRootZOrder();
        if (FAILED(hr))
        {
            if (overlay.visual)
                (void)parentVisual->RemoveVisual(overlay.visual.Get());
            overlay.effect.Reset();
            overlay.scaleTransform.Reset();
            overlay.placementTransform.Reset();
            overlay.visual.Reset();
            return false;
        }
    }

    if (!overlay.surface || overlay.surfaceWidth != width || overlay.surfaceHeight != height)
    {
        overlay.surface.Reset();
        const HRESULT hr = dcompDevice_->CreateSurface(width, height,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &overlay.surface);
        if (FAILED(hr) || !overlay.surface) return false;
        overlay.surfaceWidth = width;
        overlay.surfaceHeight = height;
    }

    overlay.bounds = bounds;
    if (!UpdateCompositionAnimationOverlayContent(overlay, cache))
    {
        overlay.surface.Reset();
        return false;
    }

    if (FAILED(overlay.SetPlacementOffset({bounds.left, bounds.top})))
        return false;
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
    const auto materialKey = reinterpret_cast<std::uintptr_t>(&overlay);
    auto& material = AnimationHighlightCompositor(overlay);
    if (material.HasSrgbContent(materialKey) &&
        !material.SetSrgbContentPose(materialKey, anchor, scale, opacity))
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
    float normalizedScaleStartSlope,
    const snowdesktop::popup_motion_curve::Timeline* springTimeline)
{
    if (!overlay.active || !overlay.visual ||
        !overlay.effect || !overlay.scaleTransform ||
        !dcompDevice_ || durationMilliseconds == 0)
        return false;
    const auto materialKey = reinterpret_cast<std::uintptr_t>(&overlay);
    auto& material = AnimationHighlightCompositor(overlay);
    const D2D1_POINT_2F localAnchor = D2D1::Point2F(
        static_cast<float>(anchor.x - overlay.bounds.left),
        static_cast<float>(anchor.y - overlay.bounds.top));
    Microsoft::WRL::ComPtr<IDCompositionAnimation> scaleAnimation;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> opacityAnimation;
    HRESULT hr = springTimeline
        ? snowdesktop::CreateCompositionMotionAnimation(dcompDevice_.Get(), *springTimeline, &scaleAnimation)
        : CreateSmoothStepAnimation(dcompDevice_.Get(), fromScale, toScale,
            durationMilliseconds, &scaleAnimation, normalizedScaleStartSlope);
    if (SUCCEEDED(hr))
    {
        hr = CreateSmoothStepAnimation(
            dcompDevice_.Get(), fromOpacity, toOpacity,
            durationMilliseconds, &opacityAnimation,
            normalizedScaleStartSlope);
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
    if (material.HasSrgbContent(materialKey) &&
        !material.StartVisualTransformAnimation(
            fromScale, toScale, fromOpacity, toOpacity, static_cast<float>(anchor.x),
            static_cast<float>(anchor.y), durationMilliseconds, normalizedScaleStartSlope,
            springTimeline, nullptr, 0, materialKey))
        return false;
    // Replace the live popup body when the snapshot begins its native track.
    return overlay.host == UiCompositionAnimationHost::FloatingPopup
        ? RenderFloatingPopupCompositionFrame() : CommitCompositionAnimationFrame();
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
    if (graphicsDeviceRecovery_.Pending())
        return false;
    if (!compositionCommitPending_)
        return true;
    snowdesktop::performance::Scope performanceScope("composition.shared", "commit");
    if (!dcompDevice_)
    {
        compositionCommitPending_ = false;
        return false;
    }
    const double commitStart =
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    const HRESULT hr = snowdesktop::startup_diagnostics::Call(L"Composition.Commit", [&] {
        return dcompDevice_->Commit();
    });
    const double contentCommitDuration =
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - commitStart;
    if (SUCCEEDED(hr))
    {
        compositionCommitPending_ = false;
        collectionPopupBackdropCompositor_.OnDirectContentCommitted(dcompDevice_.Get());
        dragPreviewHighlights_.OnDirectContentCommitted(dcompDevice_.Get());
        desktopBackdropCompositor_.OnDirectContentCommitted(dcompDevice_.Get());
        collectionPopupBackdropCompositor_.CommitDeferredFrame();
        (void)collectionPopupBackdropCompositor_.CommitSrgbContent();
        if (!dragPreviewHighlights_.CommitSrgbContent() && dragPreviewHwnd_)
            InvalidateRect(dragPreviewHwnd_, nullptr, FALSE);
        // Finish the glass update before the scene samples the live background.
        desktopBackdropCompositor_.CommitDeferredFrame();
        if (!desktopBackdropCompositor_.CommitSrgbContent() && IsWindow(hwnd_))
        {
            for (auto& [id, item] : desktopWidgetCompositionItems_)
                item.precomposedPaint = {};
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    TraceDesktopPresentation(L"composition-commit", hr);
    RecordShellHoverTrace(
        ShellHoverTraceEvent::CommitFlushed);
    uiAnimationScheduler_.RecordCommitDuration(contentCommitDuration);
    if (SUCCEEDED(hr))
        return true;

    // This device contains the desktop and floating-Dock trees. Recover those
    // surfaces without touching Quick Navigation, which owns a separate DComp
    // device specifically so its animation cannot block pointer presentation.
    compositionCommitPending_ = false;
    RecoverCompositionRenderFailure(
        L"Batched DComp Commit", hr);
    for (const auto& host : persistentDockHosts_)
        if (host && host->active)
            RecoverFloatingDockCompositionFailure(
                *host, L"Batched DComp Commit", hr);
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
    HRESULT completionHr = dcompDevice_
        ? dcompDevice_->WaitForCommitCompletion()
        : E_UNEXPECTED;
    if (FAILED(completionHr))
        completionHr = DwmFlush();
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
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message, DiagnosticLogLevel::Warning);
    return false;
}

bool DesktopApp::CommitQuickNavigationCompositionFrame()
{
    if (!quickNavigation_->dcompDevice)
        return false;
    quickNavigation_->compositionCommitPending = true;
    return true;
}

bool DesktopApp::FlushPendingQuickNavigationCompositionCommit()
{
    if (graphicsDeviceRecovery_.Pending())
        return false;
    if (!quickNavigation_->compositionCommitPending)
        return true;
    if (!quickNavigation_->dcompDevice)
    {
        quickNavigation_->compositionCommitPending = false;
        return false;
    }

    const double commitStart =
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    const HRESULT hr = quickNavigation_->dcompDevice->Commit();
    quickNavigation_->compositionCommitPending = false;
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
    namespace rules = snowdesktop::quick_navigation_animation_rules;
    auto& nav = *quickNavigation_;
    StopQuickNavigationAnimationTimeline();
    nav.animation.Advance(static_cast<std::uint64_t>(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
    ApplyQuickNavigationAnimationFrame();
    ApplyFloatingDockLayerPolicy();
    const auto effect = nav.animation.GetEffect();
    const auto key = reinterpret_cast<std::uintptr_t>(nav.hwnd);
    if (!nav.animation.IsAnimating())
    {
        if (!nav.open && nav.animation.IsHidden()) FinalizeCloseQuickNavigation();
        return true;
    }
    if (nav.animation.Resize().IsAnimating() ||
        rules::IsDeformationEffect(effect) || !nav.backdropCompositor.HasSrgbContent(key))
        return false;

    const auto visual = nav.animation.GetVisual();
    const bool opening = nav.animation.IsOpening();
    const bool responsive = nav.animation.UsesResponsiveMotion();
    const auto opacityCurve = responsive ? nav.animation.RemainingTimeline() : snowdesktop::popup_motion_curve::Timeline{};
    const auto scaleCurve = responsive && effect == rules::Effect::Scale
        ? nav.animation.RemainingScaleTimeline() : snowdesktop::popup_motion_curve::Timeline{};
    const float remaining = opening ? 1.f - visual.progress : visual.progress;
    const auto duration = static_cast<UINT>(std::max(1.0, std::ceil(responsive
        ? opacityCurve.durationMilliseconds : remaining * nav.animation.DurationMilliseconds(opening))));
    const auto start = [&](std::uintptr_t contentKey) {
        return nav.backdropCompositor.StartVisualTransformAnimation(
            visual.scale, opening ? 1.f : nav.animation.HiddenScale(), visual.opacity, opening ? 1.f : 0.f,
            static_cast<float>(nav.animationAnchorPoint.x - nav.hostRect.left),
            static_cast<float>(nav.animationAnchorPoint.y - nav.hostRect.top), duration,
            rules::SegmentNormalizedStartSlope(visual.progress, opening),
            scaleCurve.segments.empty() ? nullptr : &scaleCurve,
            responsive ? &opacityCurve : nullptr, 0, contentKey);
    };
    // The published sRGB scene and the glass belong to the same WUC compositor.
    // Stage the scene first; starting the glass root can request a commit.
    if (!start(key) || (nav.glassTheme && nav.backdropCompositor.IsAvailable() && !start(0)) ||
        !nav.backdropCompositor.CommitSrgbContent())
    {
        ApplyQuickNavigationAnimationFrame();
        return false;
    }
    nav.animationCompositorDriven = true;
    nav.animation.ResumeAt(static_cast<std::uint64_t>(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
    nav.animationCompletionToken = uiAnimationScheduler_.ScheduleOnce(duration + 2,
        [this](snowdesktop::UiScheduleToken token) {
            auto& state = *quickNavigation_;
            if (token != state.animationCompletionToken) return;
            state.animationCompletionToken = 0;
            state.animationCompositorDriven = false;
            state.animation.Advance(static_cast<std::uint64_t>(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
            if (state.animation.IsAnimating())
            {
                if (!StartQuickNavigationCompositionAnimation()) EnsureUiAnimationFrame();
            }
            else if (state.animation.IsHidden()) FinalizeCloseQuickNavigation();
            else
            {
                ApplyQuickNavigationAnimationFrame();
                ApplyFloatingDockLayerPolicy();
            }
        });
    if (nav.animationCompletionToken)
    {
        snowdesktop::performance::Value("navigation.render", "native_open_close_duration_ms", {}, duration);
        return true;
    }
    nav.animationCompositorDriven = false;
    ApplyQuickNavigationAnimationFrame();
    return false;
}

void DesktopApp::FlushNativeMenuPresentation()
{
    if (!snowdesktop::native_menu_presentation_rules::
            ShouldFlushAfterOwnerMessage(
                shellPopupMenuLayerDepth_ > 0,
                compositionPaintInProgress_,
                quickNavigation_->compositionPaintInProgress,
                IsAnyPersistentDockHostPainting()))
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
    overlay.srgbRetirementBounds = overlay.bounds;
    if (overlay.host == UiCompositionAnimationHost::FloatingPopup)
    {
        // The live renderer removes the snapshot when its replacement is
        // ready. If this paint is deferred, the snapshot remains visible.
        if (!RenderFloatingPopupCompositionFrame() &&
            floatingPopupHwnd_ &&
            IsWindow(floatingPopupHwnd_))
        {
            InvalidateRect(
                floatingPopupHwnd_, nullptr, FALSE);
        }
        return;
    }

    RECT dirty{};
    UnionRect(&dirty, &bounds, &*overlay.srgbRetirementBounds);
    if (hwnd_) InvalidateRect(hwnd_, &dirty, FALSE);
    ClearDesktopBehindCompositionAnimation(dirty);
}

void UiCompositionAnimationOverlay::FinishSrgbRetirement(
    DesktopBackdropCompositor& compositor, UiCompositionAnimationHost paintHost)
{
    if (host != paintHost || !srgbRetirementBounds)
        return;
    compositor.RemoveSrgbContent(reinterpret_cast<std::uintptr_t>(this));
    srgbRetirementBounds.reset();
}

void DesktopApp::ResetCompositionAnimationOverlay(
    UiCompositionAnimationOverlay& overlay)
{
    auto& material = AnimationHighlightCompositor(overlay);
    const auto key = reinterpret_cast<std::uintptr_t>(&overlay);
    if (!overlay.srgbRetirementBounds && material.HasSrgbContent(key))
    {
        material.RemoveSrgbContent(key);
        (void)CommitCompositionAnimationFrame();
    }
    overlay.presentingHighlights = false;
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
    if (static_cast<std::uint64_t>(overlay.surfaceWidth) * overlay.surfaceHeight * 4 > 16 * 1024 * 1024)
    {
        overlay.surface.Reset();
        overlay.surfaceWidth = overlay.surfaceHeight = 0;
    }
    overlay.bounds = {};
    overlay.active = false;
}

bool DesktopApp::UpdateCollectionPopupCompositionAnimation(
    bool commit)
{
    if (!popupAnimationOverlay_.active)
    {
        const auto* collection = GetOpenPopupWidget();
        if (!collection || !UsesCollectionPopupFan(*collection))
            ApplyCollectionPopupBackdropAnimationFrame();
        return false;
    }
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
    const bool updated = UpdateCompositionAnimationOverlay(
        popupAnimationOverlay_, visual.scale,
        anchor, visual.visible ? visual.opacity : 0.0f,
        commit);
    ApplyCollectionPopupBackdropAnimationFrame();
    return updated;
}

bool DesktopApp::UpdateLuaWidgetPanelCompositionAnimation(
    bool commit)
{
    ApplyCollectionPopupBackdropAnimationFrame();
    if (!luaWidgetPanelAnimationOverlay_.active)
        return false;
    const auto visual = luaWidgetPanelAnimation_.GetVisual();
    const POINT anchor = GetLuaWidgetPanelAnimationAnchor(GetLuaWidgetPanelRect());
    return UpdateCompositionAnimationOverlay(
        luaWidgetPanelAnimationOverlay_, visual.scale,
        anchor, visual.visible ? visual.opacity : 0.0f,
        commit);
}

bool DesktopApp::StartCollectionPopupCompositionAnimation()
{
    if (!popupAnimationOverlay_.active ||
        !popupAnimation_.IsAnimating())
        return false;
    if (popupAnimationCompletionToken_)
        uiAnimationScheduler_.Cancel(
            popupAnimationCompletionToken_);
    popupAnimationCompletionToken_ = 0;
    if (popupAnimationFrameToken_)
        uiAnimationScheduler_.Cancel(
            popupAnimationFrameToken_);
    popupAnimationFrameToken_ = 0;
    popupAnimationCompositorDriven_ = false;

    const auto visual = popupAnimation_.GetVisual();
    const bool opening = popupAnimation_.IsInteractive();
    const auto springTimeline = popupAnimation_.UsesSpring() ? popupAnimation_.RemainingTimeline() : snowdesktop::popup_motion_curve::Timeline{};
    const float normalizedScaleStartSlope =
        snowdesktop::popup_animation_rules::
            ScaleSegmentNormalizedStartSlope(
                visual.progress, opening);
    const float remaining = opening
        ? 1.0f - visual.progress : visual.progress;
    const UINT duration = popupAnimation_.UsesSpring() ? std::max<UINT>(1, static_cast<UINT>(springTimeline.durationMilliseconds)) : std::max<UINT>(
        1, static_cast<UINT>(std::lround(
            remaining * static_cast<float>(popupAnimation_.DurationMilliseconds(opening)))));
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
    // The hidden host can predate the snapshot's first complete icon mask.
    // Stage that material before the compositor receives either timeline.
    if (std::exchange(floatingPopupBackdropUpdatePending_, false))
        UpdateCollectionPopupBackdrop(false);
    if (!AnimateCompositionAnimationOverlay(
            popupAnimationOverlay_,
            visual.scale,
            opening ? 1.0f : popupAnimation_.HiddenScale(),
            anchor, visual.opacity, popupAnimation_.EndpointOpacity(opening), duration,
            normalizedScaleStartSlope, popupAnimation_.UsesSpring() ? &springTimeline : nullptr))
    {
        UpdateCollectionPopupCompositionAnimation();
        return false;
    }
    const auto* collection = GetOpenPopupWidget();
    if (collection && NeedsCollectionPopupMaterialPanel(*collection) &&
        !collectionPopupBackdropCompositor_.StartVisualTransformAnimation(
            visual.scale, opening ? 1.0f : popupAnimation_.HiddenScale(),
            visual.opacity, popupAnimation_.EndpointOpacity(opening),
            static_cast<float>(anchor.x), static_cast<float>(anchor.y),
            duration, normalizedScaleStartSlope, popupAnimation_.UsesSpring() ? &springTimeline : nullptr,
            nullptr, reinterpret_cast<std::uintptr_t>(&popupAnimation_)))
    {
        // If either animation cannot start, detach both native timelines.
        // The scheduler must own both layers during the fallback.
        UpdateCollectionPopupCompositionAnimation();
        return false;
    }
    if (!FlushPendingCompositionCommit())
    {
        UpdateCollectionPopupCompositionAnimation();
        return false;
    }

    popupAnimationCompositorDriven_ = true;
    const double started = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    popupAnimationCompletionToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            duration + 2,
            [this, duration, opening, started](snowdesktop::UiScheduleToken token) {
                if (popupAnimationCompletionToken_ != token)
                    return;
                popupAnimationCompletionToken_ = 0;
                popupAnimationCompositorDriven_ = false;
                popupAnimation_.Advance(static_cast<std::uint64_t>(
                    snowdesktop::UiAnimationScheduler::
                        MonotonicMilliseconds()));
                wchar_t message[192]{};
                swprintf_s(message,
                    L"Popup animation complete: driver=compositor opening=%d durationMs=%u elapsedMs=%.2f pendingContent=%d",
                    opening ? 1 : 0, duration,
                    snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - started,
                    popupAnimationContentPending_ ? 1 : 0);
                WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message);
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
                if (popupAnimation_.IsAnimating())
                {
                    if (StartCollectionPopupCompositionAnimation())
                        return;
                    EnsureUiAnimationFrame();
                    return;
                }
                const RECT dirty = popupAnimationCacheRect_;
                const bool onDesktop = popupAnimationOverlay_.host ==
                    UiCompositionAnimationHost::Desktop;
                // Content changes kept the original bounds during the native
                // track. Size the live host before the atomic snapshot handoff.
                UpdateFloatingPopupWindowBounds(false);
                PrepareCompositionAnimationOverlayRetirement(
                    popupAnimationOverlay_, dirty);
                ResetCollectionPopupAnimationCache();
                if (onDesktop && hwnd_ && IsWindow(hwnd_))
                    InvalidateRect(hwnd_, &dirty, FALSE);
            });
    if (!popupAnimationCompletionToken_)
    {
        popupAnimationCompositorDriven_ = false;
        UpdateCollectionPopupCompositionAnimation();
        return false;
    }
    wchar_t message[192]{};
    swprintf_s(message,
        L"Popup animation start: driver=compositor opening=%d durationMs=%u folder=%d",
        opening ? 1 : 0, duration, dockFolderPopupOpen_ ? 1 : 0);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message);
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
    luaWidgetPanelAnimationCompositorDriven_ = false;

    const auto visual = luaWidgetPanelAnimation_.GetVisual();
    const bool opening =
        luaWidgetPanelAnimation_.IsInteractive();
    const auto springTimeline = luaWidgetPanelAnimation_.UsesSpring() ? luaWidgetPanelAnimation_.RemainingTimeline() : snowdesktop::popup_motion_curve::Timeline{};
    const float normalizedScaleStartSlope =
        snowdesktop::popup_animation_rules::
            ScaleSegmentNormalizedStartSlope(
                visual.progress, opening);
    const float remaining = opening
        ? 1.0f - visual.progress : visual.progress;
    const UINT duration = luaWidgetPanelAnimation_.UsesSpring() ? std::max<UINT>(1, static_cast<UINT>(springTimeline.durationMilliseconds)) : std::max<UINT>(
        1, static_cast<UINT>(std::lround(
            remaining * static_cast<float>(luaWidgetPanelAnimation_.DurationMilliseconds(opening)))));
    const POINT anchor = GetLuaWidgetPanelAnimationAnchor(GetLuaWidgetPanelRect());
    if (!AnimateCompositionAnimationOverlay(
            luaWidgetPanelAnimationOverlay_,
            visual.scale,
            opening ? 1.0f : luaWidgetPanelAnimation_.HiddenScale(),
            anchor, visual.opacity, luaWidgetPanelAnimation_.EndpointOpacity(opening), duration,
            normalizedScaleStartSlope, luaWidgetPanelAnimation_.UsesSpring() ? &springTimeline : nullptr))
    {
        UpdateLuaWidgetPanelCompositionAnimation();
        return false;
    }
    if (collectionPopupGlassTheme_ &&
        !collectionPopupBackdropCompositor_.StartVisualTransformAnimation(
            visual.scale, opening ? 1.0f : luaWidgetPanelAnimation_.HiddenScale(),
            visual.opacity, luaWidgetPanelAnimation_.EndpointOpacity(opening),
            static_cast<float>(anchor.x), static_cast<float>(anchor.y),
            duration, normalizedScaleStartSlope, luaWidgetPanelAnimation_.UsesSpring() ? &springTimeline : nullptr,
            nullptr, reinterpret_cast<std::uintptr_t>(&luaWidgetPanelAnimation_)))
    {
        UpdateLuaWidgetPanelCompositionAnimation();
        return false;
    }
    if (!FlushPendingCompositionCommit())
    {
        UpdateLuaWidgetPanelCompositionAnimation();
        return false;
    }

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
        UpdateLuaWidgetPanelCompositionAnimation();
        return false;
    }
    return true;
}
