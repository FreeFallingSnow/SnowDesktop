#include "app/app.h"
#include "diagnostics/diagnostic_log.h"

#include <array>

HRESULT DesktopApp::SyncDesktopCompositionRootZOrder()
{
    if (!dcompVisual_)
        return E_UNEXPECTED;

    // IDCompositionVisual::AddVisual has counter-intuitive null-reference
    // semantics: insertAbove == TRUE puts the child below every sibling.
    // Reattach every managed root child with an explicit predecessor so the
    // order stays deterministic regardless of which surface was created or
    // recovered most recently.
    const auto desktopOverlayVisual = [](
        const UiCompositionAnimationOverlay& overlay)
        -> IDCompositionVisual2* {
        return snowdesktop::widget_composition_layer_rules::
                BelongsToCompositionRoot(
                    overlay.host,
                    UiCompositionAnimationHost::Desktop)
            ? overlay.visual.Get() : nullptr;
    };
    const std::array<IDCompositionVisual2*, 5> bottomToTop{
        desktopBackgroundCompositionVisual_.Get(),
        desktopWidgetCompositionLayer_.Get(),
        desktopForegroundCompositionVisual_.Get(),
        desktopOverlayVisual(popupAnimationOverlay_),
        desktopOverlayVisual(luaWidgetPanelAnimationOverlay_),
    };

    std::array<bool, 5> removedFromRoot{};
    const auto restoreRemovedPrefix = [&](std::size_t end) {
        IDCompositionVisual2* predecessor = nullptr;
        HRESULT restoreHr = S_OK;
        for (std::size_t index = 0; index < end; ++index)
        {
            IDCompositionVisual2* visual = bottomToTop[index];
            if (!visual || !removedFromRoot[index])
                continue;
            const HRESULT hr = dcompVisual_->AddVisual(
                visual, TRUE, predecessor);
            if (SUCCEEDED(hr))
                predecessor = visual;
            else if (SUCCEEDED(restoreHr))
                restoreHr = hr;
        }
        return restoreHr;
    };

    for (std::size_t index = 0;
         index < bottomToTop.size(); ++index)
    {
        IDCompositionVisual2* visual = bottomToTop[index];
        if (visual)
        {
            const HRESULT hr = dcompVisual_->RemoveVisual(visual);
            if (FAILED(hr))
            {
                const HRESULT restoreHr =
                    restoreRemovedPrefix(index);
                if (FAILED(restoreHr))
                {
                    wchar_t message[160]{};
                    wsprintfW(
                        message,
                        L"Desktop root z-order rollback FAILED "
                        L"hr=0x%08X after remove hr=0x%08X",
                        static_cast<unsigned>(restoreHr),
                        static_cast<unsigned>(hr));
                    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message, DiagnosticLogLevel::Warning);
                }
                return hr;
            }
            removedFromRoot[index] = true;
        }
    }

    IDCompositionVisual2* predecessor = nullptr;
    std::array<bool, 5> attachedToRoot{};
    HRESULT addFailure = S_OK;
    for (std::size_t index = 0;
         index < bottomToTop.size(); ++index)
    {
        IDCompositionVisual2* visual = bottomToTop[index];
        if (!visual)
            continue;
        const HRESULT hr = dcompVisual_->AddVisual(
            visual, TRUE, predecessor);
        if (FAILED(hr))
        {
            if (SUCCEEDED(addFailure))
                addFailure = hr;
            continue;
        }
        attachedToRoot[index] = true;
        predecessor = visual;
    }

    if (FAILED(addFailure))
    {
        // A transient AddVisual failure must not strand every later visual
        // outside the tree. Insert missing children in their original slots;
        // an existing predecessor keeps already-restored siblings ordered.
        predecessor = nullptr;
        HRESULT restoreHr = S_OK;
        for (std::size_t index = 0;
             index < bottomToTop.size(); ++index)
        {
            IDCompositionVisual2* visual = bottomToTop[index];
            if (!visual)
                continue;
            if (attachedToRoot[index])
            {
                predecessor = visual;
                continue;
            }
            const HRESULT hr = dcompVisual_->AddVisual(
                visual, TRUE, predecessor);
            if (SUCCEEDED(hr))
            {
                attachedToRoot[index] = true;
                predecessor = visual;
            }
            else if (SUCCEEDED(restoreHr))
            {
                restoreHr = hr;
            }
        }
        if (FAILED(restoreHr))
        {
            wchar_t message[160]{};
            wsprintfW(
                message,
                L"Desktop root z-order reattach FAILED "
                L"hr=0x%08X after add hr=0x%08X",
                static_cast<unsigned>(restoreHr),
                static_cast<unsigned>(addFailure));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Rendering, message, DiagnosticLogLevel::Warning);
        }
        return addFailure;
    }
    return S_OK;
}

bool DesktopApp::RenderDesktopForegroundComposition(
    const RECT* updateRect,
    bool hiddenMode)
{
    const RECT client = desktopOutputClientBounds_;
    const auto retiresHere = [](const UiCompositionAnimationOverlay& overlay) {
        return overlay.host == UiCompositionAnimationHost::Desktop && overlay.srgbRetirementBounds;
    };
    const bool retireCollection = retiresHere(popupAnimationOverlay_);
    const bool retireLuaPanel = retiresHere(luaWidgetPanelAnimationOverlay_);
    RECT clippedUpdate{};
    const RECT* dcompUpdate = nullptr;
    // The public presentation entry rebuilds both layers together if the
    // output geometry has changed.
    if (updateRect &&
        IntersectRect(&clippedUpdate, updateRect, &client) &&
        !IsRectEmpty(&clippedUpdate))
    {
        clippedUpdate = desktopForegroundTooltipDamage_.IncludePrevious(clippedUpdate, client);
        for (const auto* overlay : {&popupAnimationOverlay_, &luaWidgetPanelAnimationOverlay_})
        {
            RECT snapshot{};
            if (overlay->host == UiCompositionAnimationHost::Desktop && overlay->srgbRetirementBounds &&
                IntersectRect(&snapshot, &*overlay->srgbRetirementBounds, &client))
                UnionRect(&clippedUpdate, &clippedUpdate, &snapshot);
        }
        dcompUpdate = &clippedUpdate;
    }

    // A popup close may reveal a tooltip outside the requested Dock/popup
    // area. Discover that content in the normal draw, then repair the surface
    // before the caller commits content and native glass. Existing tooltips
    // are included up front, so ordinary pointer paints remain one pass.
    for (int pass = 0; pass < 3; ++pass)
    {
        desktopForegroundTooltipDamage_.BeginDraw();
        const HRESULT hr = RenderDesktopOutputScene(true, dcompUpdate, [&](ID2D1DeviceContext* context) {
            drawing_.Reset();
            DrawDesktopForeground(context, hiddenMode, dcompUpdate);
        });
        desktopForegroundTooltipDamage_.EndDraw();
        if (FAILED(hr)) return false;
        if (dcompUpdate)
        {
            const RECT required = desktopForegroundTooltipDamage_.IncludeCurrent(clippedUpdate, client);
            if (!EqualRect(&required, &clippedUpdate))
            {
                clippedUpdate = required;
                // If geometry changes again during repair, finish with one
                // bounded full draw rather than exposing glass without text.
                if (pass > 0) dcompUpdate = nullptr;
                continue;
            }
        }
        desktopForegroundTooltipDamage_.AcceptDraw();
        if (retireCollection)
            popupAnimationOverlay_.FinishSrgbRetirement(
                desktopBackdropCompositor_, UiCompositionAnimationHost::Desktop);
        if (retireLuaPanel)
            luaWidgetPanelAnimationOverlay_.FinishSrgbRetirement(
                desktopBackdropCompositor_, UiCompositionAnimationHost::Desktop);
        if (!SyncDesktopWidgetCompositionZOrder()) return false;
        // Another transaction may change interaction pixels on this same
        // allocation. Only an actual pointer frame can retain its footprint.
        if (!presentingPointerInteractionFrame_)
            ResetPointerInteractionDamage();
        return true;
    }
    return false;
}

bool DesktopApp::PresentDesktopForegroundComposition(
    const RECT& updateRect)
{
    if (graphicsDeviceRecovery_.Pending())
        return false;
    if (!hwnd_ || !IsWindow(hwnd_) || IsRectEmpty(&updateRect))
        return false;

    RECT client{};
    RECT clipped{};
    GetClientRect(hwnd_, &client);
    if (!IntersectRect(&clipped, &updateRect, &client) ||
        IsRectEmpty(&clipped))
    {
        return true;
    }

    if (ShouldShowFloatingPopupWindow())
    {
        const bool popupPresented =
            PresentFloatingPopupComposition();
        if (handlingFloatingPopupInput_)
            return popupPresented;
    }

    if (compositionPaintInProgress_)
    {
        InvalidateRect(hwnd_, &clipped, FALSE);
        desktopPointerPresentPending_ = true;
        EnsureUiAnimationFrame();
        return true;
    }

    const HRESULT initialized = CreateOrResizeCompositionSurface();
    if (FAILED(initialized))
    {
        RecoverCompositionRenderFailure(L"Desktop output surfaces", initialized);
        return false;
    }
    if (initialized == S_FALSE || std::any_of(
            desktopOutputCompositions_.begin(), desktopOutputCompositions_.end(),
            [](const auto& output) { return !output.background.initialized; }))
        return OnPaint();

    compositionPaintInProgress_ = true;
    struct PaintScope final
    {
        DesktopApp& app;
        ~PaintScope() { app.FinishDesktopCompositionPaint(); }
    } paintScope{ *this };

    desktopBackdropCompositor_.BeginFrame(false);
    if (!RenderDesktopForegroundComposition(
            &clipped, desktopIconsHidden_))
    {
        desktopBackdropCompositor_.EndFrame(false);
        RecoverCompositionRenderFailure(
            L"Desktop foreground direct update", E_FAIL);
        return false;
    }

    KeepDesktopWidgetBackdropPanels();
    desktopBackdropCompositor_.EndFrame(false);

    if (!CommitCompositionAnimationFrame())
    {
        RecoverCompositionRenderFailure(
            L"Queue desktop foreground direct update", E_FAIL);
        return false;
    }
    compositionRenderRecoveryPending_ = false;
    if (!FlushPendingCompositionCommit()) return false;
    SyncDockTitleTooltip();
    SyncPageNavHotEdgeTooltip();
    SyncLuaWidgetTooltip();
    return true;
}
