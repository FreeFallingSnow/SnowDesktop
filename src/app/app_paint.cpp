#include "app.h"
#include "desktop_backdrop_update_rules.h"

// Desktop composition paint transaction.

void DesktopApp::OnPaint(const RECT* updateRect)
{
    RecordShellHoverTrace(ShellHoverTraceEvent::PaintBegin);
    // COM calls made while resolving glass wallpaper sources may dispatch a
    // nested WM_PAINT on this same UI thread. D2D/DComp drawing is not
    // re-entrant, so defer that invalidation until the active frame ends.
    if (compositionPaintInProgress_)
    {
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, updateRect, FALSE);
        return;
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
        return;
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
        return;
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
    if (widgetPreviewActive && mouseDownWidgetIndex_ < widgets_.size())
    {
        desktopBackdropCompositor_.RemovePanel(
            GetStandaloneWidgetFrameRect(
                widgets_[mouseDownWidgetIndex_]));
    }
    desktopBackdropCompositor_.BeginFrame(completeGlassCollection);

    RenderFrame(
        context.Get(),
        forceCompleteGlassCollection ? nullptr : dcompUpdate,
        desktopIconsHidden_);

    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context.Reset();
    hr = paintSurface->EndDraw();
    if (FAILED(hr))
    {
        RecoverCompositionRenderFailure(L"Background EndDraw", hr);
        return;
    }

    // One widget surface failure is isolated and root-rendered on the next
    // paint; unrelated component surfaces remain valid.
    (void)FlushPendingDesktopWidgetComposition();

    if (!RenderDesktopForegroundComposition(
            forceCompleteGlassCollection ? nullptr : dcompUpdate,
            desktopIconsHidden_))
    {
        RecoverCompositionRenderFailure(
            L"Desktop foreground composition", E_FAIL);
        return;
    }

    if (!IsRectEmpty(&floatingDockDesktopBackdropHandoffRect_))
    {
        desktopBackdropCompositor_.KeepPanel(
            floatingDockDesktopBackdropHandoffRect_);
    }
    KeepDesktopWidgetBackdropPanels();
    desktopBackdropCompositor_.EndFrame();
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
        WriteDiagnosticLogEntry(message.c_str());
        nativeGlassPanelReadyLogged_ = true;
    }

    if (!FlushPendingWidgetMarqueeComposition())
    {
        RecoverCompositionRenderFailure(
            L"Widget marquee composition", E_FAIL);
        return;
    }
    if (!SyncWidgetMarqueeCompositionVisibility())
    {
        RecoverCompositionRenderFailure(
            L"Widget marquee visibility", E_FAIL);
        return;
    }

    if (!CommitCompositionAnimationFrame())
    {
        RecoverCompositionRenderFailure(
            L"Queue Paint Commit", E_FAIL);
        return;
    }
    compositionRenderRecoveryPending_ = false;
    if (widgetAccessibilityProvider_)
        widgetAccessibilityProvider_->RefreshEvents();
    RecordShellHoverTrace(ShellHoverTraceEvent::PaintEnd);
}
