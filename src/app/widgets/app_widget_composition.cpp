#include "app/app.h"
#include "diagnostics/diagnostic_log.h"
#include "common/utils.h"
#include "app/render/desktop_backdrop_update_rules.h"
#include "diagnostics/performance_trace.h"
#include "settings/animation_settings.h"
#include "widget/view/widget_surface_retention.h"
#include "widget/view/widget_surface_renderer.h"
#include "widget/runtime/widget_engine.h"
#include "ui/render/ui_presentation_scope.h"

#include <algorithm>

namespace
{
constexpr UINT kMaximumWidgetSurfaceDimension =
    D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

bool SameRect(const RECT& left, const RECT& right)
{
    return EqualRect(&left, &right) != FALSE;
}

struct DesktopWidgetSurfaceFailure
{
    std::wstring widgetId;
    const wchar_t* stage = L"Render";
    HRESULT hr = E_FAIL;
    bool retry = true;
};

void CloseWidgetContentClip(DesktopWidgetCompositionItem& item)
{
    item.contentVisible = false;
    if (item.clip)
    {
        (void)item.clip->SetRight(0.0f);
        (void)item.clip->SetBottom(0.0f);
    }
}

HRESULT RetireRenderedSurface(
    snowdesktop::widget_runtime::WidgetSurfaceRenderer* renderer,
    DesktopWidgetCompositionItem& item)
{
    if (renderer) renderer->Cancel(item.renderTarget);
    HRESULT result = S_OK;
    if (item.renderedVisualAttached && item.visual && item.renderedVisual)
        result = item.visual->RemoveVisual(item.renderedVisual.Get());
    item.renderedVisualAttached = false;
    item.renderTarget.reset();
    item.renderedVisual.Reset();
    return result;
}

HRESULT UpdateHoverOpacity(IDCompositionDesktopDevice* device,
    DesktopWidgetCompositionItem& item, float opacity)
{
    if (item.submittedOpacity == opacity) return S_OK;
    if (item.opacityEffect)
    {
        const HRESULT hr = item.opacityEffect->SetOpacity(opacity);
        if (FAILED(hr)) return hr;
    }
    else
    {
        Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
        HRESULT hr = device->CreateEffectGroup(&effect);
        if (SUCCEEDED(hr)) hr = effect->SetOpacity(opacity);
        if (SUCCEEDED(hr)) hr = item.visual->SetEffect(effect.Get());
        if (FAILED(hr)) return hr;
        item.opacityEffect = std::move(effect);
    }
    item.submittedOpacity = opacity;
    return S_OK;
}
}

// Every desktop widget owns a compact DirectComposition child surface. The
// root surface contains desktop icons only; a separate foreground surface
// covers widgets with Dock, popup, drag, marquee, and navigation content.

static bool CoversDesktopWidget(const std::vector<RECT>& coverage, const RECT& bounds)
{
    if (coverage.empty() || IsRectEmpty(&bounds)) return false;
    RECT frame = bounds;
    constexpr LONG border = snowdesktop::widget_composition_layer_rules::kWidgetSurfaceBorderOverdraw;
    InflateRect(&frame, border, border);
    return std::any_of(coverage.begin(), coverage.end(), [&](const RECT& covered) {
        return covered.left <= frame.left && covered.top <= frame.top &&
            covered.right >= frame.right && covered.bottom >= frame.bottom;
    });
}

void DesktopApp::UpdateDesktopWidgetCoverage(std::vector<RECT> coverage)
{
    if (std::ranges::equal(desktopWidgetCoverage_, coverage,
            [](const RECT& left, const RECT& right) { return EqualRect(&left, &right) != FALSE; }))
        return;
    const auto previous = std::exchange(desktopWidgetCoverage_, std::move(coverage));
    for (const auto& widget : widgets_)
    {
        if (IsGroupedWidget(widget)) continue;
        const RECT bounds = GetStandaloneWidgetFrameRect(widget);
        const bool wasCovered = CoversDesktopWidget(previous, bounds);
        if (wasCovered == CoversDesktopWidget(desktopWidgetCoverage_, bounds)) continue;
        if (!SyncDesktopWidgetOcclusion(widget, bounds) && hwnd_)
        {
            RECT dirty = bounds;
            InflateRect(&dirty, 3, 3);
            InvalidateRect(hwnd_, &dirty, FALSE);
        }
    }
    if (compositionCommitPending_ && !compositionPaintInProgress_ &&
        !IsAnyPersistentDockHostPainting() && !floatingPopupCompositionPaintInProgress_)
        (void)FlushPendingCompositionCommit();
}

bool DesktopApp::SyncDesktopWidgetOcclusion(const DesktopWidget& widget, const RECT& bounds)
{
    const bool covered = CoversDesktopWidget(desktopWidgetCoverage_, bounds);
    if (widgetEngine_ && (widget.type == DesktopWidgetType::LuaScript ||
            widget.type == DesktopWidgetType::LuaStack))
    {
        if (const auto* content = ResolveLuaWidgetContent(&widget))
            widgetEngine_->SetWidgetDesktopOccluded(content->id, covered);
    }
    const auto existing = desktopWidgetCompositionItems_.find(widget.id);
    if (existing == desktopWidgetCompositionItems_.end() || existing->second.occluded == covered)
        return covered;
    auto& item = existing->second;
    item.occluded = covered;
    snowdesktop::performance::Value("desktop", "widget_occluded", widget.id, covered ? 1 : 0);
    if (widgetSurfaceRenderer_)
        widgetSurfaceRenderer_->SetPaused(item.renderTarget, covered || !item.visible);
    if (covered)
    {
        pendingDesktopWidgetCompositions_.erase(widget.id);
        PauseWidgetMarqueeComposition(widget.id);
    }
    else if (item.visible)
        pendingDesktopWidgetCompositions_.insert_or_assign(widget.id, std::nullopt);
    return covered;
}

bool DesktopApp::QueueDesktopWidgetComposition(
    const std::wstring& widgetId, std::optional<RECT> damage)
{
    if (graphicsDeviceRecovery_.Pending()) return false;
    snowdesktop::performance::Scope performanceScope(
        "widget.composition", "queue", widgetId);
    const auto fail = [&]() {
        desktopWidgetCompositionFailurePending_ = true;
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    };
    const size_t memberIndex = FindWidgetIndexById(widgetId);
    if (memberIndex < widgets_.size())
    {
        const auto& owner = ResolveLuaWidgetFrameOwner(widgets_[memberIndex]);
        if (owner.id != widgetId)
            return ResolveLuaWidgetContent(&owner) == &widgets_[memberIndex]
                ? QueueDesktopWidgetComposition(owner.id, damage) : true;
    }
    if (widgetId.empty() || !dcompDevice_ || !dcompVisual_ || !hwnd_)
        return fail();

    DesktopWidget* widgetData = nullptr;
    for (auto& widget : widgets_)
    {
        if (widget.id == widgetId)
        {
            widgetData = &widget;
            break;
        }
    }
    if (!widgetData)
        return fail();

    const RECT bounds = GetStandaloneWidgetFrameRect(*widgetData);
    if (IsRectEmptyRect(bounds) || CoversDesktopWidget(desktopWidgetCoverage_, bounds))
        return true;
    if (!customDesktopVisible_ ||
        (desktopIconsHidden_ && !widgetData->keepWhenDesktopHidden))
    {
        SetDesktopWidgetCompositionVisible(widgetId, false, bounds);
        return true;
    }

    const auto existing =
        desktopWidgetCompositionItems_.find(widgetId);
    if (existing != desktopWidgetCompositionItems_.end() &&
        !existing->second.visible)
    {
        return true;
    }

    if (!desktopWidgetCompositionLayer_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &desktopWidgetCompositionLayer_);
        if (FAILED(hr) || !desktopWidgetCompositionLayer_)
            return fail();
        hr = dcompVisual_->AddVisual(
            desktopWidgetCompositionLayer_.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr))
            hr = SyncDesktopCompositionRootZOrder();
        if (FAILED(hr))
        {
            (void)dcompVisual_->RemoveVisual(
                desktopWidgetCompositionLayer_.Get());
            desktopWidgetCompositionLayer_.Reset();
            return fail();
        }
    }

    auto [position, inserted] =
        desktopWidgetCompositionItems_.try_emplace(widgetId);
    auto& item = position->second;
    if (inserted)
    {
        HRESULT hr = dcompDevice_->CreateVisual(&item.visual);
        if (SUCCEEDED(hr))
            hr = dcompDevice_->CreateRectangleClip(&item.clip);
        if (SUCCEEDED(hr)) hr = item.clip->SetLeft(0.0f);
        if (SUCCEEDED(hr)) hr = item.clip->SetTop(0.0f);
        if (SUCCEEDED(hr)) hr = item.clip->SetRight(0.0f);
        if (SUCCEEDED(hr)) hr = item.clip->SetBottom(0.0f);
        if (SUCCEEDED(hr)) hr = item.visual->SetClip(item.clip.Get());
        if (FAILED(hr) || !item.visual || !item.clip)
        {
            desktopWidgetCompositionItems_.erase(position);
            return fail();
        }
        hr = desktopWidgetCompositionLayer_->AddVisual(
            item.visual.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            desktopWidgetCompositionItems_.erase(position);
            return fail();
        }
        item.bounds = bounds;
        item.visible = true;
    }

    // A full update supersedes queued marquee ticks for the same surface.
    auto [update, queued] = pendingDesktopWidgetCompositions_.try_emplace(widgetId, damage);
    if (!queued)
    {
        if (!damage)
            update->second.reset();
        else if (update->second)
            UnionRect(&*update->second, &*update->second, &*damage);
    }
    const snowdesktop::UiImmediatePresentationScope immediatePresentation(
        desktopWidgetPointerBatchActive_);
    // Every timed widget update adds its ID before the scheduler presents the
    // owner once. Direct input and non-scheduled invalidation still present now.
    return uiAnimationScheduler_.RequestScheduledPresentation(
        &pendingDesktopWidgetCompositions_, [this] {
            return PresentQueuedDesktopWidgetComposition();
        });
}

bool DesktopApp::DeferRetainedDesktopWidgetCompositionForPopupResize()
{
    if ((!desktopWidgetPopupResizeTailToken_ &&
            (!GetCollectionPopupResizeTransition() || !IsCollectionPopupHostedByFloatingWindow())) ||
        desktopStartupPresentationPending_ || desktopWidgetCompositionFailurePending_ ||
        graphicsDeviceRecovery_.Pending() || updatingDisplayTopology_ ||
        exitRequested_ || pendingDesktopWidgetCompositions_.empty() || mouseDown_ ||
        widgetAction_ != WidgetAction::None || middleButtonWidgetMove_ ||
        marqueeActive_ || detailColumnResizeActive_ || popupScrollbarDragging_ ||
        luaWidgetPanelMouseDown_ || renameController_.IsActive() ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive())
        return false;

    // Only retire queued work later when every owner can continue showing its
    // last frame. First paints and recovery still complete on their own path.
    if (!std::ranges::all_of(pendingDesktopWidgetCompositions_, [this](const auto& update) {
            const auto found = desktopWidgetCompositionItems_.find(update.first);
            return found != desktopWidgetCompositionItems_.end() &&
                found->second.visible && found->second.contentVisible;
        }))
        return false;

    if (!desktopWidgetPopupResizeTailToken_)
    {
        // This deadline is measured from the first deferral, including across
        // retargets. Repeated navigation cannot indefinitely hold repaint work.
        constexpr UINT maximumDeferralMs = 250;
        desktopWidgetPopupResizeTailToken_ = uiAnimationScheduler_.ScheduleOnce(
            maximumDeferralMs, [this](snowdesktop::UiScheduleToken token) {
                if (desktopWidgetPopupResizeTailToken_ != token) return;
                desktopWidgetPopupResizeTailToken_ = 0;
                (void)uiAnimationScheduler_.RequestScheduledPresentation(
                    &desktopWidgetPopupResizeTailToken_, [this] {
                        return PresentQueuedDesktopWidgetComposition(false, false);
                    });
            });
        if (desktopWidgetPopupResizeTailToken_)
            snowdesktop::performance::Value("popup", "resize.desktop_widgets_deferred", {},
                static_cast<double>(pendingDesktopWidgetCompositions_.size()));
    }
    return desktopWidgetPopupResizeTailToken_ != 0;
}

bool DesktopApp::PresentQueuedDesktopWidgetComposition(bool passivePointerFrame, bool yieldToPopupResize)
{
    if (snowdesktop::widget_composition_layer_rules::
            ShouldDeferWidgetSurfaceDraw(
                compositionPaintInProgress_,
                IsAnyPersistentDockHostPainting(),
                floatingPopupCompositionPaintInProgress_,
                desktopWidgetPointerBatchActive_))
    {
        if (desktopWidgetHoverFadePending_ && hwnd_)
            InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (!passivePointerFrame && yieldToPopupResize && DeferRetainedDesktopWidgetCompositionForPopupResize())
        return true;
    if (passivePointerFrame && !pendingDesktopWidgetCompositions_.empty())
    {
        const UINT delay = snowdesktop::widget_composition_layer_rules::
            PassivePointerFrameDelay(GetTickCount64(), desktopWidgetLastPointerPresentTick_);
        if (delay != 0)
        {
            if (!desktopWidgetHoverTailToken_)
                desktopWidgetHoverTailToken_ = uiAnimationScheduler_.ScheduleOnce(
                    delay, [this](snowdesktop::UiScheduleToken token) {
                        if (desktopWidgetHoverTailToken_ != token) return;
                        desktopWidgetHoverTailToken_ = 0;
                        (void)PresentQueuedDesktopWidgetComposition();
                    });
            // A scheduler failure must fall through to the synchronous path.
            if (desktopWidgetHoverTailToken_) return true;
        }
    }
    if (desktopWidgetHoverTailToken_)
    {
        uiAnimationScheduler_.Cancel(desktopWidgetHoverTailToken_);
        desktopWidgetHoverTailToken_ = 0;
    }
    if (FlushPendingDesktopWidgetComposition() &&
        FlushPendingWidgetMarqueeComposition() &&
        SyncWidgetMarqueeCompositionVisibility() &&
        CommitCompositionAnimationFrame() &&
        FlushPendingCompositionCommit())
    {
        // Lua tooltips live outside the widget surface and need to follow local commits too.
        SyncLuaWidgetTooltip();
        return true;
    }
    desktopWidgetCompositionFailurePending_ = true;
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    return false;
}

bool DesktopApp::FlushPendingDesktopWidgetComposition(bool yieldToPopupResize)
{
    if (graphicsDeviceRecovery_.Pending()) return false;
    if (desktopWidgetPointerBatchActive_) return true;
    if (desktopWidgetHoverTailToken_) return true;
    if (yieldToPopupResize && DeferRetainedDesktopWidgetCompositionForPopupResize())
        return true;
    if (desktopWidgetPopupResizeTailToken_)
    {
        uiAnimationScheduler_.Cancel(desktopWidgetPopupResizeTailToken_);
        desktopWidgetPopupResizeTailToken_ = 0;
    }
    if (pendingDesktopWidgetCompositions_.empty())
    {
        if (desktopWidgetHoverFadePending_ && !compositionPaintInProgress_)
        {
            desktopBackdropCompositor_.BeginFrame(false);
            KeepDesktopWidgetBackdropPanels();
            desktopBackdropCompositor_.EndFrame(false);
        }
        if (SyncDesktopWidgetCompositionZOrder())
            return true;
        desktopWidgetCompositionFailurePending_ = true;
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    if (!dcompDevice_ || !desktopWidgetCompositionLayer_)
    {
        desktopWidgetCompositionFailurePending_ = true;
        return false;
    }

    // Prepare the visible startup frame on the UI device before showing the desktop.
    if (!desktopStartupPresentationPending_ && !widgetSurfaceRenderer_ &&
        !widgetSurfaceRendererUnavailable_)
    {
        auto renderer = std::make_unique<snowdesktop::widget_runtime::WidgetSurfaceRenderer>();
        if (renderer->Initialize(d2dDevice_.Get(), hwnd_, d3dDevice_.Get()))
            widgetSurfaceRenderer_ = std::move(renderer);
        else
        {
            widgetSurfaceRendererUnavailable_ = true;
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering,
                L"Widget render worker unavailable; using synchronous surfaces",
                DiagnosticLogLevel::Warning);
        }
    }

    desktopWidgetLastPointerPresentTick_ = GetTickCount64();
    auto pending = std::move(pendingDesktopWidgetCompositions_);
    snowdesktop::performance::Scope performanceScope("composition.shared", "widgets.flush");
    snowdesktop::performance::Value("composition.shared", "batch_widget_count", {},
        static_cast<double>(pending.size()));
    pendingDesktopWidgetCompositions_.clear();
    const bool ownsPaintScope = !compositionPaintInProgress_;
    if (ownsPaintScope)
    {
        compositionPaintInProgress_ = true;
        desktopBackdropCompositor_.BeginFrame(false);
    }

    bool structuralOk = true;
    std::vector<DesktopWidgetSurfaceFailure> surfaceFailures;
    for (const auto& [widgetId, damage] : pending)
    {
        auto composition = desktopWidgetCompositionItems_.find(widgetId);
        if (composition == desktopWidgetCompositionItems_.end())
            continue;
        if (!composition->second.visible || composition->second.occluded)
            continue;

        DesktopWidget* widgetData = nullptr;
        WidgetContainer* widgetContainer = nullptr;
        Widget* widgetItem = nullptr;
        for (auto& widget : widgets_)
        {
            if (widget.id == widgetId)
            {
                widgetData = &widget;
                break;
            }
        }
        if (widgetData)
        {
            for (auto& container : containers_)
            {
                auto* candidate =
                    dynamic_cast<WidgetContainer*>(container.get());
                if (candidate && candidate->GetWidgetData() == widgetData)
                {
                    widgetContainer = candidate;
                    break;
                }
            }
            if (!widgetContainer)
            {
                for (auto& ownedItem : items_oo_)
                {
                    auto* candidate =
                        dynamic_cast<Widget*>(ownedItem.get());
                    if (candidate && candidate->GetWidgetData() == widgetData)
                    {
                        widgetItem = candidate;
                        break;
                    }
                }
            }
        }
        if (!widgetData || (!widgetContainer && !widgetItem))
        {
            surfaceFailures.push_back({
                widgetId, L"Resolve owner", E_UNEXPECTED, false });
            continue;
        }

        auto* luaWidget = dynamic_cast<LuaScript*>(widgetItem);
        const auto* content = luaWidget ? ResolveLuaWidgetContent(widgetData) : nullptr;
        const std::wstring contentId = content ? content->id : widgetId;
        snowdesktop::performance::Scope widgetPerformanceScope(
            "widget.composition", "draw", contentId);

        auto& item = composition->second;
        const auto expansion = widgetHoverExpansion_.find(widgetId);
        const bool animating = widgetContainer && !widgetContainer->IsHosted() &&
            expansion != widgetHoverExpansion_.end() && IsWidgetHoverExpansionAnimating(*widgetData);
        if (!animating) item.hoverRollUnavailable = false;
        const RECT visibleBounds = GetStandaloneWidgetFrameRect(*widgetData);
        const RECT expandedBounds = animating ? GetExpandedWidgetFrameRect(*widgetData) : visibleBounds;
        const RECT collapsedBounds = animating
            ? GetCollapsedWidgetFrameRect(*widgetData, expandedBounds) : visibleBounds;
        const bool drawRollSnapshot = animating && item.backdropRequested &&
            desktopBackdropCompositor_.IsAvailable() && !item.hoverRollUnavailable &&
            collapsedBounds.bottom < expandedBounds.bottom;
        const RECT bounds = drawRollSnapshot ? expandedBounds : visibleBounds;
        const bool reserveHoverSampling = widgetContainer && !widgetContainer->IsHosted() &&
            widgetData->titleBarCollapsed && widgetData->titleBarExpandOnHover && CanFoldWidget(*widgetData);
        const RECT hoverSamplingEnvelope = reserveHoverSampling
            ? (animating ? expandedBounds : GetExpandedWidgetFrameRect(*widgetData)) : RECT{};
        const LONG rawWidth = bounds.right - bounds.left;
        const LONG rawHeight = bounds.bottom - bounds.top;
        // The fallback also keeps one allocation throughout the transition.
        const LONG surfaceHeight = expandedBounds.bottom - bounds.top;
        constexpr LONG surfaceOverdraw = static_cast<LONG>(
            snowdesktop::widget_composition_layer_rules::
                kWidgetSurfaceBorderOverdraw);
        constexpr LONG maximumLogicalSurfaceDimension =
            static_cast<LONG>(kMaximumWidgetSurfaceDimension) -
            surfaceOverdraw * 2;
        if (rawWidth <= 0 || rawHeight <= 0 ||
            rawWidth > maximumLogicalSurfaceDimension ||
            surfaceHeight > maximumLogicalSurfaceDimension)
        {
            surfaceFailures.push_back({
                widgetId, L"Validate bounds", E_INVALIDARG, false });
            continue;
        }
        const LONG surfaceLeft = static_cast<LONG>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceOrigin(bounds.left));
        const LONG surfaceTop = static_cast<LONG>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceOrigin(bounds.top));
        const UINT width = static_cast<UINT>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceExtent(rawWidth));
        const UINT height = static_cast<UINT>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceExtent(surfaceHeight));
        const RECT oldBounds = item.bounds;
        const bool boundsChanged = !SameRect(oldBounds, bounds);
        const RECT contentFrame{surfaceLeft, surfaceTop,
            surfaceLeft + static_cast<LONG>(width), surfaceTop + static_cast<LONG>(height)};
        const POINT interactionPoint = lastMousePoint_;
        const POINT paintPointer = SuppressDesktopWidgetDragTargets() ||
            (IsCollectionPopupHostedByFloatingWindow() && IsPointOccludedByOpenPopup(interactionPoint))
            ? POINT{LONG_MIN, LONG_MIN} : interactionPoint;
        RECT dirty{};
        if (damage && luaWidget && widgetEngine_ && item.surface && item.contentVisible &&
            item.presentingSrgb && !item.marqueeDamageUnavailable && !boundsChanged &&
            item.width == width && item.height == height && item.renderedContentId == contentId &&
            IntersectRect(&dirty, &*damage, &contentFrame) &&
            desktopBackdropCompositor_.HasSrgbContent(item.BackdropOwnerKey()) &&
            widgetEngine_->CanReplayDesktopMarqueeFrame(contentId, bounds,
                widgetData->gridSpan.columns, widgetData->gridSpan.rows, d2dDevice_.Get()))
        {
            snowdesktop::performance::Scope updateScope("widget.composition", "marquee.damage", contentId);
            RECT surfaceDamage = dirty;
            OffsetRect(&surfaceDamage, -surfaceLeft, -surfaceTop);
            const HRESULT updated = desktopBackdropCompositor_.UpdateSrgbScene(
                item.BackdropOwnerKey(), surfaceDamage,
                [&](ID2D1DeviceContext* context, POINT offset) {
                    context->SetDpi(96.0f, 96.0f);
                    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
                    context->SetTransform(D2D1::Matrix3x2F::Translation(
                        static_cast<float>(offset.x - dirty.left),
                        static_cast<float>(offset.y - dirty.top)));
                    context->PushAxisAlignedClip(ToD2DRect(dirty), D2D1_ANTIALIAS_MODE_ALIASED);
                    context->Clear(D2D1::ColorF(0, 0, 0, 0));
                    desktopWidgetCompositionDrawInProgress_ = true;
                    lastMousePoint_ = paintPointer;
                    // Replay the existing draw order while the surface clips
                    // writes to the damaged part. Its backdrop remains attached.
                    luaWidget->DrawCompositionSurface(
                        context, widgetData->bounds, widgetData->selected ? 2 : 0, false, nullptr);
                    lastMousePoint_ = interactionPoint;
                    desktopWidgetCompositionDrawInProgress_ = false;
                    context->PopAxisAlignedClip();
                    context->SetTransform(D2D1::Matrix3x2F::Identity());
                    drawing_.Reset();
                    return S_OK;
                });
            if (SUCCEEDED(updated))
            {
                snowdesktop::performance::Value("widget.composition", "marquee_damage_pixels", contentId,
                    static_cast<double>(dirty.right - dirty.left) * (dirty.bottom - dirty.top));
                continue;
            }
            if (updated != E_INVALIDARG)
            {
                surfaceFailures.push_back({widgetId, L"Update marquee region", updated, true});
                continue;
            }
            // Retain the full path for a surface that rejects regional draws.
            item.marqueeDamageUnavailable = true;
        }
        if (!item.surface || item.width != width || item.height != height)
        {
            ComPtr<IDCompositionSurface> surface;
            HRESULT hr = dcompDevice_->CreateSurface(
                width, height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED,
                &surface);
            if (FAILED(hr) || !surface)
            {
                surfaceFailures.push_back({
                    widgetId, L"CreateSurface",
                    FAILED(hr) ? hr : E_FAIL, true });
                continue;
            }
            item.surface = std::move(surface);
            item.width = width;
            item.height = height;
            item.marqueeDamageUnavailable = false;
            item.precomposedPaint = {};
            snowdesktop::performance::Value(
                "widget.composition", "surface_created", widgetId, 1);
        }

        const auto sceneKey = item.BackdropOwnerKey();
        const bool hasLocalBase = luaWidget && !boundsChanged && item.contentVisible &&
            item.presentingSrgb && item.renderedContentId == contentId && item.precomposedPaint.revision != 0 &&
            desktopBackdropCompositor_.HasSrgbContent(sceneKey);
        ComPtr<ID2D1DeviceContext> context;
        ComPtr<ID2D1CommandList> commands;
        // This visual already has a transparent DComp surface. Record a Lua
        // refresh without opening and clearing that whole scratch allocation.
        if (hasLocalBase)
        {
            if (!item.precomposedRecorder)
                (void)d2dDevice_->CreateDeviceContext(
                    D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &item.precomposedRecorder);
            if (item.precomposedRecorder && SUCCEEDED(item.precomposedRecorder->CreateCommandList(&commands)))
                context = item.precomposedRecorder;
        }

        POINT updateOffset{};
        HRESULT hr = S_OK;
        if (commands)
        {
            context->SetTarget(commands.Get());
            context->BeginDraw();
        }
        else
        {
            hr = item.surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                reinterpret_cast<void**>(context.ReleaseAndGetAddressOf()), &updateOffset);
            if (FAILED(hr) || !context)
            {
                surfaceFailures.push_back({
                    widgetId, L"BeginDraw", FAILED(hr) ? hr : E_FAIL, true });
                continue;
            }
        }
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x - surfaceLeft),
            static_cast<float>(updateOffset.y - surfaceTop)));
        std::optional<DesktopBackdropCompositor::SrgbSceneRecording> scene;
        if (!commands)
        {
            context->Clear(D2D1::ColorF(0, 0, 0, 0));
            scene.emplace(context.Get());
        }

        snowdesktop::widget_runtime::WidgetSurfacePaint paint;
        desktopWidgetCompositionDrawInProgress_ = true;
        item.iconBackdrops.BeginDraw();
        desktopWidgetIconBackdropsDrawing_ = &item.iconBackdrops;
        desktopWidgetBackdropRequestedDuringDraw_ = false;
        desktopWidgetBackdropCornerRadiusDuringDraw_ = 0.0f;
        desktopWidgetBackdropBlurRadiusDuringDraw_ = 0.0f;
        lastMousePoint_ = paintPointer;
        if (drawRollSnapshot)
        {
            widgetContainer->DrawHoverRollSnapshot(context.Get(), lastMousePoint_);
        }
        else if (widgetContainer)
        {
            widgetContainer->DrawChrome(context.Get(), lastMousePoint_);
        }
        else if (luaWidget)
        {
            snowdesktop::performance::Scope contentScope(
                "widget.composition", "content.record", contentId);
            struct PaintOutput
            {
                WidgetEngine* engine;
                ~PaintOutput() { if (engine) engine->SetDesktopSurfacePaintOutput(nullptr); }
            } output{widgetEngine_.get()};
            if (output.engine) output.engine->SetDesktopSurfacePaintOutput(&paint);
            luaWidget->DrawCompositionSurface(
                context.Get(), widgetData->bounds,
                widgetData->selected ? 2 : 0, true, output.engine ? &paint : nullptr);
        }
        else
        {
            widgetItem->Draw(
                context.Get(), widgetData->bounds,
                widgetData->selected ? 2 : 0);
        }
        lastMousePoint_ = interactionPoint;
        desktopWidgetIconBackdropsDrawing_ = nullptr;
        desktopWidgetCompositionDrawInProgress_ = false;
        const bool backdropRequested =
            desktopWidgetBackdropRequestedDuringDraw_;
        const int backdropCornerRadius = std::max(
            0, static_cast<int>(std::lround(
                desktopWidgetBackdropCornerRadiusDuringDraw_)));
        const int backdropBlurRadius = std::clamp(
            static_cast<int>(std::lround(
                desktopWidgetBackdropBlurRadiusDuringDraw_)), 0, 48);
        desktopWidgetBackdropRequestedDuringDraw_ = false;

        HRESULT sceneResult;
        if (commands)
        {
            sceneResult = context->EndDraw();
            context->SetTarget(nullptr);
            if (SUCCEEDED(sceneResult)) sceneResult = commands->Close();
        }
        else
        {
            sceneResult = scene->Finish();
        }
        auto* recorded = commands ? commands.Get() : scene->Scene();
        bool preparedScene = false;
        if (SUCCEEDED(sceneResult))
        {
            const auto localDamage = hasLocalBase ? paint.DamageFrom(item.precomposedPaint) : std::nullopt;
            RECT patch{};
            if (localDamage && IntersectRect(&patch, &*localDamage, &contentFrame) &&
                !SameRect(patch, contentFrame))
            {
                OffsetRect(&patch, -surfaceLeft, -surfaceTop);
                const POINT origin{updateOffset.x + patch.left, updateOffset.y + patch.top};
                preparedScene = SUCCEEDED(desktopBackdropCompositor_.UpdateSrgbScene(sceneKey, patch, recorded, origin));
                if (preparedScene)
                    snowdesktop::performance::Value("widget.composition", "local_refresh_damage_pixels", contentId,
                        static_cast<double>(patch.right - patch.left) * (patch.bottom - patch.top));
            }
            if (!preparedScene)
                preparedScene = desktopBackdropCompositor_.PublishSrgbScene(
                    hwnd_, sceneKey, contentFrame, context.Get(), updateOffset, recorded,
                    item.visible ? item.HoverOpacity() : 0.f);
        }
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();
        HRESULT endDrawHr = commands ? S_OK : item.surface->EndDraw();
        if (FAILED(sceneResult)) endDrawHr = sceneResult;
        if (!preparedScene && SUCCEEDED(endDrawHr)) endDrawHr = E_FAIL;
        drawing_.Reset();
        if (FAILED(endDrawHr))
        {
            item.iconBackdrops.DiscardDraw();
            surfaceFailures.push_back({
                widgetId, L"EndDraw", endDrawHr, true });
            continue;
        }

        // Keep the outgoing worker visual attached until the synchronous
        // replacement has finished drawing and can be committed with it.
        if (item.renderTarget)
        {
            const HRESULT retired = RetireRenderedSurface(widgetSurfaceRenderer_.get(), item);
            if (FAILED(retired))
            {
                item.iconBackdrops.DiscardDraw();
                surfaceFailures.push_back({widgetId, L"Retire worker visual", retired, true});
                continue;
            }
        }

        const bool showContent = item.visible &&
            (item.surface || item.renderedVisualAttached);
        const bool hadRoll = item.hoverRollOpening.has_value();
        const bool backdropChanged = !item.backdropRegistered || boundsChanged ||
            item.backdropCornerRadius != backdropCornerRadius ||
            item.backdropBlurRadius != backdropBlurRadius ||
            (hadRoll && !drawRollSnapshot);
        item.backdropRequested = backdropRequested;
        if (showContent && backdropRequested)
        {
            if (backdropChanged)
            {
                item.backdropRegistered =
                    desktopBackdropCompositor_.AddPanel(
                        bounds,
                        static_cast<float>(backdropCornerRadius),
                        static_cast<float>(backdropBlurRadius), item.BackdropOwnerKey(), true);
            }
            item.backdropCornerRadius = backdropCornerRadius;
            item.backdropBlurRadius = backdropBlurRadius;
        }
        else if (item.backdropRegistered)
        {
            (void)desktopBackdropCompositor_.RemovePanel(oldBounds, item.BackdropOwnerKey());
            item.backdropRegistered = false;
        }

        const bool srgbPresented = showContent;
        hr = desktopBackdropCompositor_.SetDirectContentForSrgb(
            item.BackdropOwnerKey(), srgbPresented, dcompDevice_.Get(), item.visual.Get(), item.surface.Get());
        if (SUCCEEDED(hr))
            item.presentingSrgb = srgbPresented;
        if (SUCCEEDED(hr))
            hr = item.visual->SetOffsetX(static_cast<float>(surfaceLeft));
        if (SUCCEEDED(hr))
            hr = item.visual->SetOffsetY(static_cast<float>(surfaceTop));

        bool rollInstalled = false;
        if (SUCCEEDED(hr) && drawRollSnapshot && item.backdropRegistered)
        {
            const bool opening = expansion->second.IsInteractive();
            rollInstalled = !backdropChanged && item.hoverRollOpening == opening;
            if (!rollInstalled)
            {
                rollInstalled = StartDesktopWidgetHoverRoll(item, bounds,
                    collapsedBounds.bottom - collapsedBounds.top,
                    static_cast<float>(backdropCornerRadius), expansion->second.RemainingTimeline());
                if (rollInstalled)
                    expansion->second.RebaseAnimationClock(static_cast<std::uint64_t>(
                        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
            }
            if (rollInstalled) item.hoverRollOpening = opening;
        }
        item.bounds = rollInstalled ? bounds : visibleBounds;
        if (!rollInstalled)
        {
            item.hoverRollOpening.reset();
            if (desktopBackdropCompositor_.HasSrgbContent(item.BackdropOwnerKey()))
                desktopBackdropCompositor_.SetSrgbContentRollClip(item.BackdropOwnerKey(),
                    drawRollSnapshot ? visibleBounds : RECT{}, static_cast<float>(backdropCornerRadius));
            if (drawRollSnapshot)
            {
                // Keep both visuals at the current sampled height if either
                // compositor cannot install the paired animation.
                item.hoverRollUnavailable = true;
                if (item.backdropRegistered)
                {
                    item.backdropRegistered = desktopBackdropCompositor_.AddPanel(
                        visibleBounds, static_cast<float>(backdropCornerRadius),
                        static_cast<float>(backdropBlurRadius), item.BackdropOwnerKey(), true);
                    if (!item.backdropRegistered)
                        (void)desktopBackdropCompositor_.RemovePanel(bounds, item.BackdropOwnerKey());
                }
            }
            if (SUCCEEDED(hr) && (hadRoll || drawRollSnapshot))
                hr = item.visual->SetClip(item.clip.Get());
        }

        const float clipHeight = static_cast<float>(
            snowdesktop::widget_composition_layer_rules::WidgetSurfaceExtent(
                item.bounds.bottom - item.bounds.top));
        if (SUCCEEDED(hr))
            hr = item.clip->SetRight(showContent ? static_cast<float>(width) : 0.0f);
        if (SUCCEEDED(hr))
            hr = item.clip->SetBottom(showContent ? clipHeight : 0.0f);
        if (FAILED(hr))
        {
            item.iconBackdrops.DiscardDraw();
            surfaceFailures.push_back({
                widgetId, L"Update visual", hr, true });
        }
        else
        {
            item.contentVisible = showContent;
            if (luaWidget)
            {
                item.renderedContentId = contentId;
                item.precomposedPaint = paint.revision != 0 && paint.appearance
                    ? std::move(paint) : snowdesktop::widget_runtime::WidgetSurfacePaint{};
            }
            if (showContent && item.hoverFade && item.hoverFade->waitingForPixels)
                StartDesktopWidgetHoverFade(item, true);
            if (item.backdropRegistered)
                desktopBackdropCompositor_.SetPanelAnimationFence(item.BackdropOwnerKey(), hoverSamplingEnvelope);
            if (showContent)
            {
                const float fileIconBlur = dynamic_cast<ScrollingItemWidget*>(widgetContainer) &&
                    iconBeautifySettings_.enabled && iconBeautifySettings_.glassEnabled
                    ? iconBeautifySettings_.glassBlurRadius : 0.f;
                item.iconBackdrops.Publish(desktopBackdropCompositor_, item.HoverOpacity(),
                    item.backdropRegistered ? item.BackdropOwnerKey() : 0, d2dDevice_.Get(), fileIconBlur);
            }
            else
                item.iconBackdrops.DiscardDraw();
        }
    }

    if (!SyncDesktopWidgetCompositionZOrder())
    {
        desktopWidgetCompositionFailurePending_ = true;
        structuralOk = false;
    }

    // A child surface can fail independently (for example after a stale
    // BeginDraw). Recreate only that surface and preserve the desktop root,
    // foreground, sibling widgets and popup host. A device-wide or tree
    // failure still reaches the structural recovery path above/at Commit.
    for (const auto& failure : surfaceFailures)
    {
        if (RequestGraphicsDeviceRecovery(failure.stage, failure.hr))
            continue;
        wchar_t message[256]{};
        wsprintfW(message,
            L"Desktop widget %s FAILED hr=0x%08X; %s child surface",
            failure.stage,
            static_cast<unsigned>(failure.hr),
            failure.retry ? L"recreating" : L"dropping");
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message, DiagnosticLogLevel::Warning);

        const auto position =
            desktopWidgetCompositionItems_.find(failure.widgetId);
        if (position == desktopWidgetCompositionItems_.end())
            continue;
        if (!failure.retry)
        {
            // Retire child marquees and queued work with the failed owner,
            // just as an explicit widget removal does.
            RemoveDesktopWidgetComposition(failure.widgetId, true);
            continue;
        }

        auto& item = position->second;
        if (!desktopBackdropCompositor_.HasSrgbContent(item.BackdropOwnerKey()))
        {
            item.presentingSrgb = false;
            CloseWidgetContentClip(item);
        }
        (void)RetireRenderedSurface(widgetSurfaceRenderer_.get(), item);
        item.surface.Reset();
        item.precomposedRecorder.Reset();
        item.precomposedPaint = {};
        item.width = 0;
        item.height = 0;
        pendingDesktopWidgetCompositions_.insert_or_assign(failure.widgetId, std::nullopt);
        if (hwnd_ && IsWindow(hwnd_))
        {
            RECT dirty = item.bounds;
            InflateRect(&dirty, 1, 1);
            InvalidateRect(hwnd_, &dirty, FALSE);
        }
    }
    if (ownsPaintScope)
    {
        KeepDesktopWidgetBackdropPanels();
        desktopBackdropCompositor_.EndFrame(false);
        compositionPaintInProgress_ = false;
    }
    if (!structuralOk && hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    return structuralOk;
}

void DesktopApp::DrainDesktopWidgetSurfaceResults()
{
    if (!widgetSurfaceRenderer_) return;
    auto results = widgetSurfaceRenderer_->Drain();
    bool changed = false;
    for (const auto& result : results)
    {
        const auto position = desktopWidgetCompositionItems_.find(result.owner);
        if (position == desktopWidgetCompositionItems_.end() ||
            position->second.renderTarget != result.target) continue;
        auto& item = position->second;
        HRESULT hr = result.status;
        if (SUCCEEDED(hr) && item.visible)
        {
            // Replace the pixels beneath the native marquee children. Keep
            // the existing clip, which may be closed pending a geometry draw.
            hr = item.visual->AddVisual(item.renderedVisual.Get(), TRUE, nullptr);
            if (SUCCEEDED(hr))
                item.renderedVisualAttached = true;
            if (SUCCEEDED(hr)) hr = item.visual->SetContent(nullptr);
            if (SUCCEEDED(hr))
                item.surface.Reset();
            changed = true;
        }
        if (FAILED(hr))
        {
            item.renderFailed = true;
            (void)RetireRenderedSurface(widgetSurfaceRenderer_.get(), item);
            if (!item.surface)
            {
                CloseWidgetContentClip(item);
                item.width = item.height = 0;
            }
            if (RequestGraphicsDeviceRecovery(L"Widget render worker", hr)) return;
            const auto message = L"Widget render worker failed; using synchronous surface: " +
                result.owner + L" hr=" + std::to_wstring(hr);
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering,
                message.c_str(), DiagnosticLogLevel::Warning);
            if (item.visible) pendingDesktopWidgetCompositions_.insert_or_assign(result.owner, std::nullopt);
            changed = true;
        }
    }
    if (changed) (void)PresentQueuedDesktopWidgetComposition();
}

bool DesktopApp::HasDesktopWidgetComposition(
    const std::wstring& widgetId) const
{
    return desktopWidgetCompositionItems_.contains(widgetId);
}

bool DesktopApp::DesktopWidgetHoverFadeEnabled() const
{
    namespace motion = snowdesktop::animation;
    return generalSettings_.widgetHoverVisibilityEffect == motion::Fade &&
        motion::RuntimeAnimationsEnabled();
}

bool DesktopApp::SyncDesktopWidgetPresentationVisibility(
    const DesktopWidget& widget, const RECT& bounds, bool visible, bool animate)
{
    auto position = desktopWidgetCompositionItems_.find(widget.id);
    if (position == desktopWidgetCompositionItems_.end() && animate && visible)
    {
        // Create the child before drawing so its first pixels start at zero opacity.
        (void)QueueDesktopWidgetComposition(widget.id);
        position = desktopWidgetCompositionItems_.find(widget.id);
    }
    if (position == desktopWidgetCompositionItems_.end()) return visible;
    auto& item = position->second;
    if (!animate)
        item.hoverFade.reset();
    else
    {
        const bool pixelsVisible = item.contentVisible;
        if (!item.hoverFade)
        {
            auto& fade = item.hoverFade.emplace();
            fade.motion.Configure(true, snowdesktop::animation::RuntimeDurationScale());
            if (pixelsVisible) fade.motion.ShowImmediately();
        }
        auto& fade = *item.hoverFade;
        if (visible != fade.motion.IsInteractive())
        {
            if (visible && !pixelsVisible)
                fade.waitingForPixels = true;
            else
                StartDesktopWidgetHoverFade(item, visible);
        }
        if (!visible) fade.waitingForPixels = false;
        visible = visible || fade.motion.IsAnimating();
    }
    SetDesktopWidgetCompositionVisible(widget.id, visible, bounds, widget.titleBarExpandOnHover);
    return visible;
}

void DesktopApp::StartDesktopWidgetHoverFade(DesktopWidgetCompositionItem& item, bool visible)
{
    auto& fade = *item.hoverFade;
    fade.waitingForPixels = false;
    fade.compositorFadingOut = false;
    fade.motion.Configure(true, snowdesktop::animation::RuntimeDurationScale());
    const auto tick = static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds());
    if (visible) fade.motion.Open(tick);
    else fade.motion.Close(tick);
    desktopWidgetHoverFadePending_ = true;
    if (!fade.motion.IsAnimating() || desktopWidgetHoverFadeFrameToken_) return;
    desktopWidgetHoverFadeFrameToken_ = uiAnimationScheduler_.StartAnimation(
        snowdesktop::UiAnimationSurface::Desktop,
        [this](double now) { return AdvanceDesktopWidgetHoverFades(now); });
    if (!desktopWidgetHoverFadeFrameToken_)
    {
        if (visible) fade.motion.ShowImmediately();
        else fade.motion.ResetHidden();
    }
}

bool DesktopApp::AdvanceDesktopWidgetHoverFades(double nowMilliseconds)
{
    const bool enabled = DesktopWidgetHoverFadeEnabled();
    const auto tick = static_cast<std::uint64_t>(nowMilliseconds);
    bool active = false;
    bool changed = false;
    for (auto& [_, item] : desktopWidgetCompositionItems_)
    {
        if (!item.hoverFade || !item.hoverFade->motion.IsAnimating()) continue;
        auto& motion = item.hoverFade->motion;
        if (enabled) motion.Advance(tick);
        else if (motion.IsInteractive()) motion.ShowImmediately();
        else motion.ResetHidden();
        active |= motion.IsAnimating();
        changed |= !item.hoverFade->compositorFadingOut || !motion.IsAnimating();
        if (motion.IsHidden() && hwnd_)
        {
            // Let the scene retire the zero-opacity child and its retained pixels.
            RECT dirty = item.bounds;
            InflateRect(&dirty, 3, 3);
            InvalidateRect(hwnd_, &dirty, FALSE);
        }
    }
    if (changed)
    {
        desktopWidgetHoverFadePending_ = true;
        (void)uiAnimationScheduler_.RequestScheduledPresentation(
            &pendingDesktopWidgetCompositions_, [this] {
                return PresentQueuedDesktopWidgetComposition();
            });
    }
    if (!active) desktopWidgetHoverFadeFrameToken_ = 0;
    return active;
}

void DesktopApp::SetDesktopWidgetCompositionVisible(
    const std::wstring& widgetId,
    bool visible,
    const RECT& bounds,
    bool retainOnHeightChange)
{
    const auto position = desktopWidgetCompositionItems_.find(widgetId);
    if (position == desktopWidgetCompositionItems_.end())
        return;
    auto& item = position->second;
    // A restored/resized surface must publish fresh pixels before its clip
    // opens. A passive hover tail can otherwise defer the draw while this
    // frame commits the retained surface at its previous size.
    const bool refreshBeforeShow = visible &&
        (!item.visible || !EqualRect(&item.bounds, &bounds));
    if (refreshBeforeShow)
    {
        pendingDesktopWidgetCompositions_.insert_or_assign(widgetId, std::nullopt);
        if (desktopWidgetHoverTailToken_)
        {
            uiAnimationScheduler_.Cancel(desktopWidgetHoverTailToken_);
            desktopWidgetHoverTailToken_ = 0;
        }
        // Keep the old, anchored hover frame until the new child pixels are
        // ready, even if a pointer batch defers its draw past this commit.
        const bool keepPresentedFrame = retainOnHeightChange && item.contentVisible &&
            item.surface && item.bounds.left == bounds.left &&
            item.bounds.top == bounds.top && item.bounds.right == bounds.right;
        const bool keepSrgbFrame = item.visible &&
            desktopBackdropCompositor_.HasSrgbContent(item.BackdropOwnerKey());
        if (item.clip && !keepPresentedFrame && !keepSrgbFrame)
        {
            CloseWidgetContentClip(item);
        }
    }
    if (item.visible == visible)
        return;
    item.visible = visible;
    item.hiddenSince = visible ? 0 : GetTickCount64();
    if (visible)
    {
        // Ownership survives eviction. Rebuild even when this widget lies
        // outside the current dirty rectangle; hidden data may have changed.
        pendingDesktopWidgetCompositions_.insert_or_assign(widgetId, std::nullopt);
        if (widgetSurfaceRenderer_)
            widgetSurfaceRenderer_->SetPaused(item.renderTarget, item.occluded);
    }
    else if (GetDesktopWidgetSurfaceBytes(widgetId) != 0)
    {
        hiddenWidgetSurfaceMaintenancePending_ = true;
    }
    if (!visible && item.backdropRegistered)
    {
        (void)desktopBackdropCompositor_.RemovePanel(item.bounds, item.BackdropOwnerKey());
        item.backdropRegistered = false;
    }
    if (!visible)
    {
        const auto index = FindWidgetIndexById(widgetId);
        if (index < widgets_.size())
            widgets_[index].keepInitialComposition = false;
        desktopBackdropCompositor_.RemoveSrgbContent(item.BackdropOwnerKey());
        item.presentingSrgb = false;
        item.hoverFade.reset();
        if (desktopIconsHidden_ && item.renderedVisualAttached && widgetSurfaceRenderer_)
        {
            // Keep the presented body behind the closed clip. A brief desktop
            // hide can show the same pixels while the worker refreshes them.
            widgetSurfaceRenderer_->SetPaused(item.renderTarget, true);
        }
        else
        {
            // Reject late ready messages from a target that never appeared.
            (void)RetireRenderedSurface(widgetSurfaceRenderer_.get(), item);
            item.renderedContentId.clear();
        }
        item.iconBackdrops.Hide(desktopBackdropCompositor_);
    }
    if (!visible && item.clip)
    {
        if (item.hoverRollOpening.has_value())
        {
            (void)item.visual->SetClip(item.clip.Get());
            item.hoverRollOpening.reset();
        }
        item.hoverRollUnavailable = false;
    }
    if (!visible)
    {
        CloseWidgetContentClip(item);
        // Retaining pixels briefly must not keep a native animation running
        // behind a zero-sized clip. The next draw uses the retained phase/time.
        const auto marquees = widgetMarqueeCompositionItems_.find(widgetId);
        if (marquees != widgetMarqueeCompositionItems_.end())
        {
            for (auto& [_, marquee] : marquees->second)
            {
                if (marquee.textVisual && marquee.scrolling &&
                    FAILED(marquee.textVisual->SetOffsetX(0.0f)))
                {
                    desktopWidgetCompositionFailurePending_ = true;
                    if (hwnd_ && IsWindow(hwnd_))
                        InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
        }
    }
}

void DesktopApp::KeepDesktopWidgetBackdropPanels()
{
    for (auto& [id, item] : desktopWidgetCompositionItems_)
    {
        if (!item.visible || !item.contentVisible)
        {
            if (item.backdropRegistered)
                (void)desktopBackdropCompositor_.RemovePanel(item.bounds, item.BackdropOwnerKey());
            item.backdropRegistered = false;
            item.iconBackdrops.Hide(desktopBackdropCompositor_);
            continue;
        }
        const float opacity = item.HoverOpacity();
        if (item.presentingSrgb && !desktopBackdropCompositor_.HasSrgbContent(item.BackdropOwnerKey()))
        {
            if (SUCCEEDED(item.visual->SetContent(item.surface.Get())))
            {
                desktopBackdropCompositor_.RemoveSrgbContent(item.BackdropOwnerKey());
                item.presentingSrgb = false;
                pendingDesktopWidgetCompositions_.insert_or_assign(id, std::nullopt);
            }
        }
        if (desktopBackdropCompositor_.HasSrgbContent(item.BackdropOwnerKey()))
            desktopBackdropCompositor_.SetSrgbContentOpacity(item.BackdropOwnerKey(), opacity);
        const HRESULT opacityResult = UpdateHoverOpacity(dcompDevice_.Get(), item, opacity);
        if (FAILED(opacityResult) &&
            !RequestGraphicsDeviceRecovery(L"Desktop widget hover opacity", opacityResult))
        {
            desktopWidgetCompositionFailurePending_ = true;
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (item.hoverRollOpening.has_value())
        {
            item.backdropRegistered = desktopBackdropCompositor_.KeepPanel(
                item.bounds, item.BackdropOwnerKey());
            // Recreate a lost animated panel with its clip in the next child
            // draw. Restoring only its full static geometry would flash.
            if (!item.backdropRegistered && item.visible)
            {
                pendingDesktopWidgetCompositions_.insert_or_assign(id, std::nullopt);
                if (hwnd_) InvalidateRect(hwnd_, &item.bounds, FALSE);
            }
        }
        else
        {
            item.backdropRegistered = snowdesktop::desktop_backdrop_update_rules::
                KeepOrRestoreWidgetPanel(desktopBackdropCompositor_, item);
        }
        if (item.backdropRegistered)
        {
            RECT animationFence{};
            const auto index = FindWidgetIndexById(id);
            if (index < widgets_.size())
            {
                const auto& widget = widgets_[index];
                if (widget.titleBarCollapsed && widget.titleBarExpandOnHover && CanFoldWidget(widget))
                    animationFence = GetExpandedWidgetFrameRect(widget);
            }
            desktopBackdropCompositor_.SetPanelAnimationFence(item.BackdropOwnerKey(), animationFence);
            if (item.opacityEffect)
                (void)desktopBackdropCompositor_.SetPanelOpacity(
                    item.bounds, opacity, item.BackdropOwnerKey());
        }
        item.iconBackdrops.Restore(desktopBackdropCompositor_, opacity,
            item.backdropRegistered ? item.BackdropOwnerKey() : 0, d2dDevice_.Get());
        if (item.hoverFade && item.hoverFade->motion.IsClosing())
            item.hoverFade->compositorFadingOut = SubmitDesktopWidgetFadeOut(item);
    }
    desktopWidgetHoverFadePending_ = false;
}

void DesktopApp::LogDesktopWidgetBackdropState(const wchar_t* phase) const
{
    const ULONGLONG tick = GetTickCount64();
    size_t requested = 0;
    size_t registered = 0;
    for (const auto& [widgetId, item] : desktopWidgetCompositionItems_)
    {
        requested += item.visible && item.backdropRequested ? 1 : 0;
        registered += item.visible && item.backdropRegistered ? 1 : 0;
        RECT screenBounds = item.bounds;
        MapWindowPoints(hwnd_, nullptr,
            reinterpret_cast<POINT*>(&screenBounds), 2);
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        const bool monitorKnown = GetMonitorInfoW(
            MonitorFromRect(&screenBounds, MONITOR_DEFAULTTONULL), &monitor) != FALSE;
        wchar_t message[768]{};
        swprintf_s(message,
            L"Startup glass widget: pid=%lu tick=%llu phase=%.32ls id=%.128ls "
            L"visible=%d requested=%d registered=%d "
            L"bounds=(%ld,%ld,%ld,%ld) screen=(%ld,%ld,%ld,%ld) "
            L"monitor=%ls primary=%d blur=%d",
            GetCurrentProcessId(), tick, phase, widgetId.c_str(),
            item.visible, item.backdropRequested, item.backdropRegistered,
            item.bounds.left, item.bounds.top, item.bounds.right, item.bounds.bottom,
            screenBounds.left, screenBounds.top, screenBounds.right, screenBounds.bottom,
            monitorKnown ? monitor.szDevice : L"unknown",
            monitorKnown && (monitor.dwFlags & MONITORINFOF_PRIMARY) != 0,
            item.backdropBlurRadius);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message);
    }
    wchar_t summary[256]{};
    swprintf_s(summary,
        L"Startup glass summary: pid=%lu tick=%llu phase=%ls "
        L"available=%d requested=%zu registered=%zu panels=%zu",
        GetCurrentProcessId(), tick, phase, desktopBackdropCompositor_.IsAvailable(),
        requested, registered, desktopBackdropCompositor_.PanelCount());
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, summary);
}

std::uint64_t DesktopApp::GetDesktopWidgetSurfaceBytes(
    const std::wstring& widgetId) const
{
    std::uint64_t bytes = 0;
    const auto parent = desktopWidgetCompositionItems_.find(widgetId);
    if (parent != desktopWidgetCompositionItems_.end() &&
        (parent->second.surface || parent->second.renderTarget))
        bytes = static_cast<std::uint64_t>(parent->second.width) *
            parent->second.height * 4;
    const auto marquees = widgetMarqueeCompositionItems_.find(widgetId);
    if (marquees != widgetMarqueeCompositionItems_.end())
    {
        for (const auto& [_, item] : marquees->second)
            if (item.surface)
                bytes += static_cast<std::uint64_t>(item.surfaceWidth) *
                    item.surfaceHeight * 4;
    }
    return bytes;
}

void DesktopApp::TrimHiddenDesktopWidgetSurfaces()
{
    if (!hiddenWidgetSurfaceMaintenancePending_ || !dcompDevice_ ||
        snowdesktop::widget_composition_layer_rules::ShouldDeferWidgetSurfaceDraw(
            compositionPaintInProgress_, IsAnyPersistentDockHostPainting(),
            floatingPopupCompositionPaintInProgress_))
        return;

    namespace retention = snowdesktop::widget_surface_retention;
    std::vector<std::wstring> owners;
    std::vector<retention::Candidate> candidates;
    for (const auto& [id, item] : desktopWidgetCompositionItems_)
    {
        if (item.visible)
            continue;
        const auto bytes = GetDesktopWidgetSurfaceBytes(id);
        if (bytes == 0)
            continue;
        owners.push_back(id);
        candidates.push_back({false, item.hiddenSince, bytes});
    }
    const auto releases = retention::SelectReleases(candidates, GetTickCount64());
    hiddenWidgetSurfaceMaintenancePending_ = candidates.size() > releases.size();
    bool changed = false;
    for (const auto& release : releases)
    {
        const auto& id = owners[release.index];
        auto& item = desktopWidgetCompositionItems_.at(id);
        HRESULT hr = S_OK;
        if (item.renderTarget)
        {
            hr = RetireRenderedSurface(widgetSurfaceRenderer_.get(), item);
            changed = true;
        }
        if (SUCCEEDED(hr) && item.surface)
        {
            // Resetting our ComPtr alone leaves the visual's reference alive.
            hr = item.visual->SetContent(nullptr);
            if (SUCCEEDED(hr))
            {
                item.surface.Reset();
                changed = true;
            }
        }
        if (!item.surface && !item.renderTarget)
        {
            item.width = item.height = 0;
            item.renderedContentId.clear();
        }
        const auto marquees = widgetMarqueeCompositionItems_.find(id);
        if (SUCCEEDED(hr) && marquees != widgetMarqueeCompositionItems_.end())
        {
            for (auto& [_, marquee] : marquees->second)
            {
                if (!marquee.surface)
                    continue;
                hr = marquee.textVisual->SetContent(nullptr);
                if (SUCCEEDED(hr))
                    hr = marquee.textVisual->SetOffsetX(0.0f);
                if (FAILED(hr))
                    break;
                marquee.surface.Reset();
                marquee.surfaceWidth = marquee.surfaceHeight = 0;
                // Keep phase/time and the lightweight visual hierarchy so a
                // fresh text draw resumes at the current logical position.
                changed = true;
            }
        }
        const auto remaining = GetDesktopWidgetSurfaceBytes(id);
        const auto freed = candidates[release.index].bytes - remaining;
        widgetSurfaceReclaimedBytes_ += freed;
        if (remaining == 0)
        {
            ++widgetSurfaceReclaimCount_;
            pendingDesktopWidgetCompositions_.erase(id);
            pendingWidgetMarqueeCompositions_.erase(id);
        }
        snowdesktop::performance::Value("widget.composition",
            release.reason == retention::Reason::Expired
                ? "surface_reclaimed_expired_bytes" : "surface_reclaimed_budget_bytes",
            id, static_cast<double>(freed));
        if (FAILED(hr))
        {
            hiddenWidgetSurfaceMaintenancePending_ = true;
            desktopWidgetCompositionFailurePending_ = true;
            const auto message = L"[WidgetComposition] Hidden surface detach failed: " +
                id + L" hr=" + std::to_wstring(hr);
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message.c_str(), DiagnosticLogLevel::Error);
            if (hwnd_ && IsWindow(hwnd_))
                InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    if (changed)
    {
        (void)CommitCompositionAnimationFrame();
        (void)FlushPendingCompositionCommit();
    }
}

void DesktopApp::RemoveDesktopWidgetComposition(
    const std::wstring& widgetId,
    bool invalidateRoot)
{
    pendingDesktopWidgetCompositions_.erase(widgetId);
    pendingWidgetMarqueeCompositions_.erase(widgetId);
    widgetMarqueeCompositionItems_.erase(widgetId);
    const auto position = desktopWidgetCompositionItems_.find(widgetId);
    if (position == desktopWidgetCompositionItems_.end())
        return;
    const RECT bounds = position->second.bounds;
    const auto backdropOwner = position->second.BackdropOwnerKey();
    desktopBackdropCompositor_.RemoveSrgbContent(backdropOwner);
    position->second.iconBackdrops.Hide(desktopBackdropCompositor_);
    // Query by owner even when cached registration was lost during recovery.
    (void)desktopBackdropCompositor_.RemovePanel(bounds, backdropOwner);
    (void)RetireRenderedSurface(widgetSurfaceRenderer_.get(), position->second);
    if (desktopWidgetCompositionLayer_ && position->second.visual)
    {
        (void)desktopWidgetCompositionLayer_->RemoveVisual(
            position->second.visual.Get());
    }
    desktopWidgetCompositionItems_.erase(position);
    desktopWidgetCompositionZOrder_.clear();
    if (desktopWidgetCompositionItems_.empty())
    {
        if (dcompVisual_ && desktopWidgetCompositionLayer_)
        {
            (void)dcompVisual_->RemoveVisual(
                desktopWidgetCompositionLayer_.Get());
        }
        desktopWidgetCompositionLayer_.Reset();
    }
    if (invalidateRoot && hwnd_ && IsWindow(hwnd_))
    {
        RECT dirty = bounds;
        InflateRect(&dirty, 3, 3);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
    if (!compositionPaintInProgress_ && dcompDevice_)
    {
        (void)CommitCompositionAnimationFrame();
        (void)FlushPendingCompositionCommit();
    }
}

void DesktopApp::PruneDesktopWidgetCompositions()
{
    std::unordered_set<std::wstring> liveWidgetIds;
    liveWidgetIds.reserve(widgets_.size());
    for (const auto& widget : widgets_)
        liveWidgetIds.insert(widget.id);

    std::vector<std::wstring> removedWidgetIds;
    for (const auto& [widgetId, _] : desktopWidgetCompositionItems_)
    {
        if (!liveWidgetIds.contains(widgetId))
            removedWidgetIds.push_back(widgetId);
    }
    for (const auto& widgetId : removedWidgetIds)
        RemoveDesktopWidgetComposition(widgetId, false);
}

bool DesktopApp::SyncDesktopWidgetCompositionZOrder()
{
    std::vector<std::wstring> desired;
    desired.reserve(desktopWidgetCompositionItems_.size());
    for (const auto& widget : widgets_)
    {
        if (desktopWidgetCompositionItems_.contains(widget.id))
            desired.push_back(widget.id);
    }
    std::vector<std::uintptr_t> srgbOrder;
    for (const auto& tile : desktopSrgbTiles_)
        srgbOrder.push_back(tile.key);
    for (const auto& widgetId : desired)
    {
        const auto& item = desktopWidgetCompositionItems_.at(widgetId);
        if (item.presentingSrgb) srgbOrder.push_back(item.BackdropOwnerKey());
    }
    for (const auto& tile : foregroundSrgbTiles_) srgbOrder.push_back(tile.key);
    for (const auto* overlay : {&popupAnimationOverlay_, &luaWidgetPanelAnimationOverlay_})
        if (overlay->presentingHighlights && overlay->host == UiCompositionAnimationHost::Desktop)
            srgbOrder.push_back(reinterpret_cast<std::uintptr_t>(overlay));
    desktopBackdropCompositor_.SetSrgbContentOrder(srgbOrder);
    if (!desktopWidgetCompositionLayer_)
        return desktopWidgetCompositionItems_.empty();
    if (desired == desktopWidgetCompositionZOrder_)
        return true;

    for (const auto& [_, item] : desktopWidgetCompositionItems_)
    {
        if (item.visual)
            (void)desktopWidgetCompositionLayer_->RemoveVisual(
                item.visual.Get());
    }
    for (const auto& widgetId : desired)
    {
        auto& item = desktopWidgetCompositionItems_.at(widgetId);
        const HRESULT hr = desktopWidgetCompositionLayer_->AddVisual(
            item.visual.Get(), TRUE, nullptr);
        if (FAILED(hr))
            return false;
    }
    desktopWidgetCompositionZOrder_ = std::move(desired);
    return true;
}

void DesktopApp::ResetDesktopWidgetComposition()
{
    // Join before resetting devices, command resources or the parent tree.
    widgetSurfaceRenderer_.reset();
    widgetSurfaceRendererUnavailable_ = false;
    if (desktopWidgetHoverTailToken_)
        uiAnimationScheduler_.Cancel(desktopWidgetHoverTailToken_);
    desktopWidgetHoverTailToken_ = 0;
    if (desktopWidgetPopupResizeTailToken_)
        uiAnimationScheduler_.Cancel(desktopWidgetPopupResizeTailToken_);
    desktopWidgetPopupResizeTailToken_ = 0;
    if (desktopWidgetHoverFadeFrameToken_)
        uiAnimationScheduler_.Cancel(desktopWidgetHoverFadeFrameToken_);
    desktopWidgetHoverFadeFrameToken_ = 0;
    desktopWidgetHoverFadePending_ = false;
    desktopWidgetLastPointerPresentTick_ = 0;
    // Marquee visuals belong to these parents. Retire their cache and queued
    // submissions even when only WM_SIZE/topology (not the device) is reset;
    // otherwise a later draw reuses children detached from the visible tree.
    ResetWidgetMarqueeComposition();
    presentedWidgetDragFeedback_ = {};
    pendingDesktopWidgetCompositions_.clear();
    for (const auto& [_, item] : desktopWidgetCompositionItems_)
    {
        desktopBackdropCompositor_.RemoveSrgbContent(item.BackdropOwnerKey());
        item.iconBackdrops.Hide(desktopBackdropCompositor_);
        if (item.backdropRegistered)
            (void)desktopBackdropCompositor_.RemovePanel(item.bounds, item.BackdropOwnerKey());
    }
    desktopWidgetCompositionItems_.clear();
    desktopWidgetCompositionZOrder_.clear();
    if (dcompVisual_ && desktopWidgetCompositionLayer_)
    {
        (void)dcompVisual_->RemoveVisual(
            desktopWidgetCompositionLayer_.Get());
    }
    desktopWidgetCompositionLayer_.Reset();
    desktopWidgetCompositionDrawInProgress_ = false;
    desktopWidgetIconBackdropsDrawing_ = nullptr;
    hiddenWidgetSurfaceMaintenancePending_ = false;
    desktopWidgetBackdropRequestedDuringDraw_ = false;
    desktopWidgetCompositionFailurePending_ = false;
}
