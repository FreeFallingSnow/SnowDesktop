#include "app/app.h"
#include "app/render/desktop_highlight_tiles.h"
#include "common/utils.h"
#include "diagnostics/diagnostic_log.h"
#include "app/render/desktop_backdrop_update_rules.h"
#include "widget/view/widget_accessibility_provider.h"
#include "diagnostics/performance_trace.h"
#include "dock/dock_appbar.h"

// Desktop composition paint transaction.

namespace highlight_tiles = snowdesktop::desktop_highlight_tiles;

HRESULT DesktopApp::RenderDesktopOutputScene(
    bool foreground, const RECT* updateRect,
    const std::function<void(ID2D1DeviceContext*)>& draw)
{
    // A synchronous Windows callback can invalidate the desktop while drawing.
    // Keep the exact native surfaces alive until EndDraw.
    const auto outputs = desktopOutputCompositions_;
    if (outputs.empty()) return E_UNEXPECTED;
    const auto layer = [foreground](auto& output) -> auto& {
        return foreground ? output.foreground : output.background;
    };
    const bool initializing = std::any_of(outputs.begin(), outputs.end(),
        [&](const auto& output) { return !layer(output).initialized; });
    auto& tiles = foreground ? foregroundSrgbTiles_ : desktopSrgbTiles_;
    std::vector<DesktopSrgbTile> candidates;
    struct PendingTiles
    {
        DesktopBackdropCompositor& compositor;
        const std::vector<DesktopSrgbTile>& retained;
        const std::vector<DesktopSrgbTile>& candidates;
        bool published = false;
        ~PendingTiles()
        {
            if (published) return;
            for (const auto& candidate : candidates)
                if (std::none_of(retained.begin(), retained.end(),
                        [&](const auto& previous) { return previous.key == candidate.key; }))
                    compositor.RemoveSrgbContent(candidate.key);
        }
    } pending{desktopBackdropCompositor_, tiles, candidates};
    struct OutputPaint
    {
        const DesktopOutputComposition& output;
        RECT dirty;
    };
    std::vector<OutputPaint> paints;
    for (const auto& output : outputs)
    {
        RECT dirty = output.bounds;
        if (!initializing && updateRect && !IntersectRect(&dirty, &output.bounds, updateRect))
            continue;
        paints.push_back({output, dirty});
    }
    for (const auto& paint : paints)
    {
        const auto& [output, dirty] = paint;
        RECT localDirty = dirty;
        OffsetRect(&localDirty, -output.bounds.left, -output.bounds.top);

        const auto& surface = layer(output).surface;
        ComPtr<ID2D1DeviceContext> context;
        POINT offset{};
        HRESULT hr = surface->BeginDraw(&localDirty, IID_PPV_ARGS(&context), &offset);
        if (FAILED(hr)) return hr;
        if (!context)
        {
            (void)surface->EndDraw();
            return E_UNEXPECTED;
        }
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->Clear(D2D1::ColorF(0, 0, 0, 0));

        if (&paint == &paints.front())
        {
            // Record in desktop client coordinates once. This keeps the
            // existing ordering and side effects even for a cross-screen draw.
            DesktopBackdropCompositor::SrgbSceneRecording scene(context.Get());
            if (scene.Scene()) draw(context.Get());
            hr = scene.Finish();
            if (SUCCEEDED(hr))
            {
                for (const auto& target : paints)
                {
                    const RECT& patch = target.dirty;
                    if (!highlight_tiles::Stage(desktopBackdropCompositor_, hwnd_, context.Get(),
                            scene.Scene(), target.output.bounds, patch, {patch.left, patch.top},
                            tiles, nextDesktopSrgbKey_, candidates))
                    {
                        hr = E_FAIL;
                        break;
                    }
                }
            }
        }
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();
        drawing_.Reset();
        const HRESULT endResult = surface->EndDraw();
        if (FAILED(hr)) return hr;
        if (FAILED(endResult)) return endResult;
        snowdesktop::performance::Value("desktop",
            foreground ? "output_foreground_dirty_pixels" : "output_background_dirty_pixels", {},
            static_cast<double>(dirty.right - dirty.left) * (dirty.bottom - dirty.top));
    }

    if (!std::ranges::equal(outputs, desktopOutputCompositions_,
            [&](const auto& before, const auto& after) {
                return layer(before).surface.Get() == layer(after).surface.Get();
            }))
        return E_ABORT;
    if (initializing)
        for (auto& output : desktopOutputCompositions_) layer(output).initialized = true;
    const bool full = initializing || !updateRect;
    highlight_tiles::Publish(desktopBackdropCompositor_,
        full ? desktopOutputClientBounds_ : *updateRect, full, candidates, tiles);
    pending.published = true;
    return S_OK;
}

bool DesktopApp::OnPaint(const RECT* updateRect)
{
    TraceDesktopPresentation(L"paint-enter", S_OK, updateRect);
    // Shell enumeration and COM initialization can pump WM_PAINT before the
    // model and widget engine are complete. One prepared first frame replaces
    // those partial bootstrap frames; runtime paints keep their normal path.
    if (graphicsDeviceRecovery_.Pending())
        return false;
    if (!startupInitializationComplete_)
        return false;
    if (widgetGroupTransition_.ShouldDeferPaint())
        return false; // The dispatch boundary invalidates and paints the final model.
    snowdesktop::performance::Scope performanceScope("desktop", "paint");
    if (updateRect)
        snowdesktop::performance::Value("desktop", "dirty_pixels", {},
            static_cast<double>(std::max(0L, updateRect->right - updateRect->left)) *
            std::max(0L, updateRect->bottom - updateRect->top));
    RecordShellHoverTrace(ShellHoverTraceEvent::PaintBegin);
    // COM calls made while resolving glass wallpaper sources may dispatch a
    // nested WM_PAINT on this same UI thread. D2D/DComp drawing is not
    // re-entrant, so defer that invalidation until the active frame ends.
    if (compositionPaintInProgress_)
    {
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, updateRect, FALSE);
        return false;
    }
    if (desktopWidgetCompositionFailurePending_)
    {
        desktopWidgetCompositionFailurePending_ = false;
        RecoverCompositionRenderFailure(
            L"Desktop widget composition", E_FAIL);
        return false;
    }
    compositionPaintInProgress_ = true;
    struct PaintScope final
    {
        DesktopApp& app;
        ~PaintScope() { app.FinishDesktopCompositionPaint(); }
    } paintScope{ *this };

    HRESULT hr = CreateOrResizeCompositionSurface();
    if (FAILED(hr))
    {
        RecoverCompositionRenderFailure(
            L"CreateOrResizeCompositionSurface", hr);
        return false;
    }
    PruneDesktopWidgetCompositions();

    RECT clientRect{};
    GetClientRect(hwnd_, &clientRect);
    RECT clippedUpdate{};
    const RECT* dcompUpdate = nullptr;
    if (hr != S_FALSE && updateRect &&
        IntersectRect(&clippedUpdate, updateRect, &clientRect) &&
        !IsRectEmpty(&clippedUpdate))
    {
        dcompUpdate = &clippedUpdate;
    }

    const bool widgetPreviewActive =
        widgetAction_ == WidgetAction::Move ||
        widgetAction_ == WidgetAction::Resize;
    const bool desktopMarqueeActive =
        marqueeActive_ &&
        !marqueeDockFolderPopup_ &&
        marqueeWidgetIndex_ >= widgets_.size();
    const bool forceCompleteGlassCollection =
        desktopBackdropFullCollectionPending_;
    snowdesktop::performance::Value("desktop", "backdrop_full_requested_for_paint", {},
        forceCompleteGlassCollection ? 1.0 : 0.0);
    const bool completeGlassCollection =
        snowdesktop::desktop_backdrop_update_rules::
            ShouldCollectAllPanels(
                forceCompleteGlassCollection,
                dragSession_.IsActive(),
                widgetPreviewActive,
                desktopMarqueeActive,
                dcompUpdate,
                clientRect);
    bool backdropFrameStarted = false;
    const auto beginBackdropFrame = [&] {
        desktopBackdropCompositor_.BeginFrame(completeGlassCollection, dcompUpdate ? dcompUpdate : &clientRect);
        backdropFrameStarted = true;
        if (widgetPreviewActive && mouseDownWidgetIndex_ < widgets_.size() && !luaStackMemberDrag_.Armed())
        {
            desktopBackdropCompositor_.RemovePanel(
                GetStandaloneWidgetFrameRect(
                    widgets_[mouseDownWidgetIndex_]));
        }
    };
    hr = RenderDesktopOutputScene(false, dcompUpdate, [&](ID2D1DeviceContext* context) {
        // A surface or command-list setup failure must leave the old glass in place.
        beginBackdropFrame();
        RenderFrame(context, forceCompleteGlassCollection ? nullptr : dcompUpdate, desktopIconsHidden_);
    });
    if (FAILED(hr))
    {
        if (backdropFrameStarted) desktopBackdropCompositor_.CancelFrame();
        RecoverCompositionRenderFailure(L"Desktop output background", hr,
            CompositionRecoveryScope::DesktopOutputs);
        return false;
    }
    // A dirty rectangle in a gap between physical outputs does not invoke the draw callback.
    if (!backdropFrameStarted) beginBackdropFrame();

    if (!FlushPendingDesktopWidgetComposition(true) ||
        desktopWidgetCompositionFailurePending_)
    {
        desktopWidgetCompositionFailurePending_ = false;
        RecoverCompositionRenderFailure(
            L"Desktop widget composition", E_FAIL);
        return false;
    }

    if (!RenderDesktopForegroundComposition(
            forceCompleteGlassCollection ? nullptr : dcompUpdate,
            desktopIconsHidden_))
    {
        RecoverCompositionRenderFailure(
            L"Desktop foreground composition", E_FAIL);
        return false;
    }

    KeepDesktopWidgetBackdropPanels();
    std::vector<RECT> dockBackdropFrames;
    if (generalSettings_.dockEnabled && dockSettings_.UsesReservedSpaceBackdrop() && dockAppBars_)
    {
        for (const auto& [monitor, state] : observedMonitorWindows_)
        {
            if (!state.maximized) continue;
            auto area = dockAppBars_->ApprovedArea(monitor);
            if (!area) continue;
            OffsetRect(&*area, -virtualLeft_, -virtualTop_);
            RECT clipped{};
            if (IntersectRect(&clipped, &*area, &clientRect))
                dockBackdropFrames.push_back(clipped);
        }
    }
    constexpr float kDockReservedSpaceBlur = 5.0f;
    desktopBackdropCompositor_.SetBackgroundPanels(dockBackdropFrames, kDockReservedSpaceBlur);
    desktopBackdropCompositor_.EndFrame(false);
    if (forceCompleteGlassCollection)
        desktopBackdropFullCollectionPending_ = false;
    if (!nativeGlassPanelReadyLogged_ &&
        desktopBackdropCompositor_.IsAvailable() &&
        desktopBackdropCompositor_.PanelCount() > 0)
    {
        std::wstring message =
            L"Native desktop CompositionBackdropBrush active, panels=";
        message += std::to_wstring(
            desktopBackdropCompositor_.PanelCount());
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message.c_str());
        nativeGlassPanelReadyLogged_ = true;
    }

    if (!FlushPendingWidgetMarqueeComposition())
    {
        RecoverCompositionRenderFailure(
            L"Widget marquee composition", E_FAIL);
        return false;
    }
    if (!SyncWidgetMarqueeCompositionVisibility())
    {
        RecoverCompositionRenderFailure(
            L"Widget marquee visibility", E_FAIL);
        return false;
    }

    if (!CommitCompositionAnimationFrame())
    {
        RecoverCompositionRenderFailure(
            L"Queue Paint Commit", E_FAIL);
        return false;
    }
    // The floating popup can commit the shared backdrop controller when its
    // bounds change. Submit the completed desktop pair before touching it.
    if (!FlushPendingCompositionCommit())
        return false;
    compositionRenderRecoveryPending_ = false;
    if (widgetAccessibilityProvider_)
        widgetAccessibilityProvider_->RefreshEvents();
    UpdateFloatingPopupWindowBounds(false);
    SyncDockTitleTooltip();
    SyncPageNavHotEdgeTooltip();
    SyncLuaWidgetTooltip();
    if (desktopIconsHidden_) ResetPageNotify();
    RecordShellHoverTrace(ShellHoverTraceEvent::PaintEnd);
    TraceDesktopPresentation(L"paint-queued", S_OK, updateRect);
    return true;
}
