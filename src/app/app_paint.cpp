#include "app.h"
#include "desktop_backdrop_update_rules.h"

// Desktop composition paint transaction.

void DesktopApp::OnPaint(const RECT* updateRect)
    {
        RecordShellHoverTrace(
            ShellHoverTraceEvent::PaintBegin);
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
            RecoverCompositionRenderFailure(L"CreateOrResizeCompositionSurface", hr);
            return;
        }

        RECT clientRect{};
        GetClientRect(hwnd_, &clientRect);
        RECT clippedUpdate{};
        const RECT* dcompUpdate = nullptr;
        if (updateRect && IntersectRect(&clippedUpdate, updateRect, &clientRect) &&
            !IsRectEmpty(&clippedUpdate))
            dcompUpdate = &clippedUpdate;

        // Keep the exact surface used by BeginDraw alive locally. Explorer can
        // synchronously broadcast shell messages from COM calls made during a
        // frame; a deferred recovery may replace dcompSurface_ before this
        // function reaches EndDraw.
        ComPtr<IDCompositionSurface> paintSurface = dcompSurface_;
        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        hr = paintSurface->BeginDraw(dcompUpdate, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&rawContext), &updateOffset);
        if (FAILED(hr))
        {
            RecoverCompositionRenderFailure(L"BeginDraw", hr);
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
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const bool widgetPreviewActive =
            widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize;
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
                GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]));
        }
        desktopBackdropCompositor_.BeginFrame(completeGlassCollection);

        if (!desktopIconsHidden_ || HasRetainedElements())
            RenderFrame(
                context.Get(),
                forceCompleteGlassCollection ? nullptr : dcompUpdate,
                desktopIconsHidden_);
        if (desktopIconsHidden_ && showHiddenHint_)
            DrawHiddenHintOverlay(context.Get());

        if (showWidgetAddedHint_)
            DrawWidgetAddedHintOverlay(context.Get());

        if (!IsRectEmpty(
                &floatingDockDesktopBackdropHandoffRect_))
        {
            // The D2D ownership switch and the native backdrop switch use
            // different composition APIs. Retain the source glass panel for
            // this one desktop frame so the shared backdrop compositor can
            // transfer its opacity atomically afterward.
            desktopBackdropCompositor_.KeepPanel(
                floatingDockDesktopBackdropHandoffRect_);
        }
        desktopBackdropCompositor_.EndFrame();
        if (forceCompleteGlassCollection)
            desktopBackdropFullCollectionPending_ = false;
        if (!nativeGlassPanelReadyLogged_ &&
            desktopBackdropCompositor_.IsAvailable() &&
            desktopBackdropCompositor_.PanelCount() > 0)
        {
            std::wstring message = L"Native desktop CompositionBackdropBrush active, panels=";
            message += std::to_wstring(desktopBackdropCompositor_.PanelCount());
            WriteDiagnosticLogEntry(message.c_str());
            nativeGlassPanelReadyLogged_ = true;
        }

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();

        hr = paintSurface->EndDraw();
        if (FAILED(hr))
        {
            RecoverCompositionRenderFailure(L"EndDraw", hr);
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
        RecordShellHoverTrace(
            ShellHoverTraceEvent::PaintEnd);
    }
