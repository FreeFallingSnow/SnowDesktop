#include "app.h"

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
        if (FAILED(hr))
        {
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

    if (!IsRectEmpty(&floatingDockDesktopBackdropHandoffRect_))
    {
        (void)desktopBackdropCompositor_.KeepPanel(
            floatingDockDesktopBackdropHandoffRect_);
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
