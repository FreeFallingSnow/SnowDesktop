#include "app.h"

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
        desktopWidgetCompositionLayer_.Get(),
        desktopForegroundCompositionVisual_.Get(),
        desktopOverlayVisual(pageNotifyAnimationOverlay_),
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
                    WriteDiagnosticLogEntry(message);
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
            WriteDiagnosticLogEntry(message);
        }
        return addFailure;
    }
    return S_OK;
}

HRESULT DesktopApp::CreateOrResizeDesktopForegroundCompositionSurface()
{
    if (!dcompDevice_ || !dcompVisual_ || !hwnd_)
        return E_UNEXPECTED;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT width = static_cast<UINT>(
        std::max<LONG>(1, client.right - client.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(1, client.bottom - client.top));

    if (!desktopForegroundCompositionVisual_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &desktopForegroundCompositionVisual_);
        if (FAILED(hr) || !desktopForegroundCompositionVisual_)
            return FAILED(hr) ? hr : E_FAIL;
        hr = dcompVisual_->AddVisual(
            desktopForegroundCompositionVisual_.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr))
            hr = SyncDesktopCompositionRootZOrder();
        if (FAILED(hr))
        {
            (void)dcompVisual_->RemoveVisual(
                desktopForegroundCompositionVisual_.Get());
            desktopForegroundCompositionVisual_.Reset();
            return hr;
        }
    }

    if (desktopForegroundCompositionSurface_ &&
        desktopForegroundCompositionWidth_ == width &&
        desktopForegroundCompositionHeight_ == height)
    {
        return S_OK;
    }

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr) || !surface)
        return FAILED(hr) ? hr : E_FAIL;
    hr = desktopForegroundCompositionVisual_->SetContent(surface.Get());
    if (FAILED(hr))
        return hr;

    desktopForegroundCompositionSurface_ = std::move(surface);
    desktopForegroundCompositionWidth_ = width;
    desktopForegroundCompositionHeight_ = height;
    return S_OK;
}

bool DesktopApp::RenderDesktopForegroundComposition(
    const RECT* updateRect,
    bool hiddenMode)
{
    if (FAILED(CreateOrResizeDesktopForegroundCompositionSurface()) ||
        !desktopForegroundCompositionSurface_)
    {
        return false;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT clippedUpdate{};
    const RECT* dcompUpdate = nullptr;
    if (updateRect &&
        IntersectRect(&clippedUpdate, updateRect, &client) &&
        !IsRectEmpty(&clippedUpdate))
    {
        dcompUpdate = &clippedUpdate;
    }

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    HRESULT hr = desktopForegroundCompositionSurface_->BeginDraw(
        dcompUpdate, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext), &updateOffset);
    if (FAILED(hr) || !rawContext)
        return false;

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

    brushCache_.clear();
    brushCacheContext_ = context.Get();
    DrawDesktopForeground(context.Get(), hiddenMode);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = desktopForegroundCompositionSurface_->EndDraw();
    return SUCCEEDED(hr);
}

bool DesktopApp::PresentDesktopForegroundComposition(
    const RECT& updateRect)
{
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

    compositionPaintInProgress_ = true;
    struct PaintScope final
    {
        bool& active;
        ~PaintScope() { active = false; }
    } paintScope{ compositionPaintInProgress_ };

    desktopBackdropCompositor_.BeginFrame(false);
    if (!RenderDesktopForegroundComposition(
            &clipped, desktopIconsHidden_))
    {
        desktopBackdropCompositor_.EndFrame();
        RecoverCompositionRenderFailure(
            L"Desktop foreground direct update", E_FAIL);
        return false;
    }

    KeepDesktopWidgetBackdropPanels();
    desktopBackdropCompositor_.EndFrame();

    if (!CommitCompositionAnimationFrame())
    {
        RecoverCompositionRenderFailure(
            L"Queue desktop foreground direct update", E_FAIL);
        return false;
    }
    compositionRenderRecoveryPending_ = false;
    return FlushPendingCompositionCommit();
}

void DesktopApp::ResetDesktopForegroundComposition()
{
    desktopForegroundCompositionSurface_.Reset();
    desktopForegroundCompositionWidth_ = 0;
    desktopForegroundCompositionHeight_ = 0;
    if (dcompVisual_ && desktopForegroundCompositionVisual_)
    {
        (void)dcompVisual_->RemoveVisual(
            desktopForegroundCompositionVisual_.Get());
    }
    desktopForegroundCompositionVisual_.Reset();
}
