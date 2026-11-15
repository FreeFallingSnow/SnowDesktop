#include "app/app.h"
#include "common/utils.h"
#include "desktop/display_color_mode.h"
#include "diagnostics/diagnostic_log.h"
#include "app/render/desktop_backdrop_update_rules.h"
#include "widget/view/widget_accessibility_provider.h"
#include "diagnostics/performance_trace.h"
#include "dock/dock_appbar.h"

// Desktop composition paint transaction.

namespace
{
bool Intersects(const RECT& first, const RECT& second)
{
    RECT overlap{};
    return IntersectRect(&overlap, &first, &second) != FALSE;
}

void MergeSrgbTiles(std::vector<RECT>& frames)
{
    for (std::size_t i = 0; i < frames.size();)
    {
        const auto overlap = std::find_if(frames.begin() + i + 1, frames.end(),
            [&](const RECT& frame) { return Intersects(frames[i], frame); });
        if (overlap == frames.end())
        {
            ++i;
            continue;
        }
        UnionRect(&frames[i], &frames[i], &*overlap);
        frames.erase(overlap);
        i = 0;
    }
}
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
        bool& active;
        ~PaintScope() { active = false; }
    } paintScope{ compositionPaintInProgress_ };

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
    if (updateRect &&
        IntersectRect(&clippedUpdate, updateRect, &clientRect) &&
        !IsRectEmpty(&clippedUpdate))
    {
        // Repaint the whole retained tile before falling back or replacing it.
        // Expand transitively if the union reaches another tile.
        bool expanded;
        do
        {
            const RECT before = clippedUpdate;
            for (const auto& tile : desktopSrgbTiles_)
                if (Intersects(clippedUpdate, tile.frame))
                    UnionRect(&clippedUpdate, &clippedUpdate, &tile.frame);
            expanded = !EqualRect(&before, &clippedUpdate);
        } while (expanded);
        IntersectRect(&clippedUpdate, &clippedUpdate, &clientRect);
        dcompUpdate = &clippedUpdate;
    }

    // Keep the exact surface used by BeginDraw alive locally. Explorer can
    // synchronously broadcast shell messages from COM calls made during a
    // frame; a deferred recovery may replace dcompSurface_ before EndDraw.
    ComPtr<IDCompositionSurface> paintSurface = dcompSurface_;
    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = paintSurface->BeginDraw(
        dcompUpdate, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext), &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverCompositionRenderFailure(L"Background BeginDraw", hr);
        return false;
    }

    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    const LONG updateLeft = dcompUpdate ? dcompUpdate->left : 0;
    const LONG updateTop = dcompUpdate ? dcompUpdate->top : 0;
    context->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(updateOffset.x - updateLeft),
        static_cast<float>(updateOffset.y - updateTop)));
    context->Clear(D2D1::ColorF(0, 0, 0, 0));

    const bool widgetPreviewActive =
        widgetAction_ == WidgetAction::Move ||
        widgetAction_ == WidgetAction::Resize;
    const bool desktopMarqueeActive =
        marqueeActive_ &&
        !marqueeDockFolderPopup_ &&
        marqueeWidgetIndex_ >= widgets_.size();
    const bool forceCompleteGlassCollection =
        desktopBackdropFullCollectionPending_;
    const bool completeGlassCollection =
        snowdesktop::desktop_backdrop_update_rules::
            ShouldCollectAllPanels(
                forceCompleteGlassCollection,
                dragSession_.IsActive(),
                widgetPreviewActive,
                desktopMarqueeActive,
                dcompUpdate,
                clientRect);
    desktopBackdropCompositor_.BeginFrame(completeGlassCollection);
    if (widgetPreviewActive && mouseDownWidgetIndex_ < widgets_.size() && !luaStackMemberDrag_.Armed())
    {
        desktopBackdropCompositor_.RemovePanel(
            GetStandaloneWidgetFrameRect(
                widgets_[mouseDownWidgetIndex_]));
    }
    std::vector<RECT> srgbDraws;
    {
        struct CollectSrgb
        {
            std::vector<RECT>*& slot;
            ~CollectSrgb() { slot = nullptr; }
        } collect{desktopSrgbTileDraws_};
        if (!desktopIconsHidden_ && !dragSession_.IsActive() && !desktopMarqueeActive && !widgetPreviewActive &&
            snowdesktop::display_color::AnyWideColorMonitor())
            desktopSrgbTileDraws_ = &srgbDraws;
        RenderFrame(context.Get(), forceCompleteGlassCollection ? nullptr : dcompUpdate, desktopIconsHidden_);
    }

    MergeSrgbTiles(srgbDraws);
    const RECT dirty = dcompUpdate ? *dcompUpdate : clientRect;
    std::vector<DesktopSrgbTile> srgbCandidates;
    for (RECT frame : srgbDraws)
    {
        if (!IntersectRect(&frame, &frame, &clientRect) || !Intersects(frame, dirty)) continue;
        RECT visible{};
        IntersectRect(&visible, &frame, &dirty);
        if (!EqualRect(&frame, &visible))
        {
            InvalidateRect(hwnd_, &frame, FALSE);
            continue;
        }
        const auto old = std::find_if(desktopSrgbTiles_.begin(), desktopSrgbTiles_.end(),
            [&](const auto& tile) { return EqualRect(&tile.frame, &frame) != FALSE; });
        const auto key = old == desktopSrgbTiles_.end() ? nextDesktopSrgbKey_ : old->key;
        const POINT atlas{updateOffset.x + frame.left - updateLeft,
            updateOffset.y + frame.top - updateTop};
        if (!desktopBackdropCompositor_.StageSrgbContent(hwnd_, key, frame, context.Get(), atlas))
            continue;
        if (old == desktopSrgbTiles_.end()) nextDesktopSrgbKey_ += 2;
        srgbCandidates.push_back({key, frame});
        context->PushAxisAlignedClip(ToD2DRect(frame), D2D1_ANTIALIAS_MODE_ALIASED);
        context->Clear(D2D1::ColorF(0, 0.f));
        context->PopAxisAlignedClip();
    }

    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context.Reset();
    hr = paintSurface->EndDraw();
    if (FAILED(hr))
    {
        RecoverCompositionRenderFailure(L"Background EndDraw", hr);
        return false;
    }
    std::erase_if(desktopSrgbTiles_, [&](const auto& old) {
        if (!Intersects(old.frame, dirty)) return false;
        if (std::none_of(srgbCandidates.begin(), srgbCandidates.end(),
            [&](const auto& next) { return next.key == old.key; }))
            desktopBackdropCompositor_.RemoveSrgbContent(old.key);
        return true;
    });
    for (const auto& tile : srgbCandidates)
    {
        if (!desktopBackdropCompositor_.PublishSrgbContent(tile.key))
        {
            RecoverCompositionRenderFailure(L"Publish desktop sRGB tile", E_FAIL);
            return false;
        }
        desktopSrgbTiles_.push_back(tile);
    }

    if (!FlushPendingDesktopWidgetComposition() ||
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
