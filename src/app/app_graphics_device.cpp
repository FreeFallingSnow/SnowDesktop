#include "app.h"
#include "dock_taskbar_diagnostics.h"

// Graphics-device and composition-surface lifecycle.

HRESULT DesktopApp::InitGraphicsDevices()
{
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dImmediateContext;
    ComPtr<ID2D1Factory1> d2dFactory = d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<IDCompositionDesktopDevice> dcompDevice;
    // D3D11
    D3D_FEATURE_LEVEL fl{};
    bool usingWarp = false;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice, &fl, nullptr);
    if (FAILED(hr))
    {
        usingWarp = true;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3dDevice, &fl, nullptr);
    }
    {
        wchar_t buf[128];
        wsprintfW(buf, L"D3D11 driver=%s hr=0x%08X feature=0x%04X",
            usingWarp ? L"WARP" : L"HARDWARE",
            static_cast<unsigned>(hr), static_cast<unsigned>(fl));
        WriteDiagnosticLogEntry(buf);
    }
    if (FAILED(hr)) return hr;
    d3dDevice->GetImmediateContext(&d3dImmediateContext);
    if (!d3dImmediateContext) return E_FAIL;

    // The factory and its geometries are device-independent. Keep their
    // identity across GPU loss; only the D2D device/context must be replaced.
    if (!d2dFactory)
    {
        D2D1_FACTORY_OPTIONS factoryOptions{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &factoryOptions,
            reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return hr;
    {
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter &&
            SUCCEEDED(adapter->GetDesc(&desc)))
        {
            wchar_t buf[256];
            wsprintfW(buf, L"D3D adapter=%s vendor=0x%04X device=0x%04X",
                desc.Description, desc.VendorId, desc.DeviceId);
            WriteDiagnosticLogEntry(buf);
        }
    }
    hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    if (FAILED(hr)) return hr;
    hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);
    if (FAILED(hr)) return hr;

    // DComp — create from the D2D device for interop
    hr = DCompositionCreateDevice2(d2dDevice.Get(), __uuidof(IDCompositionDesktopDevice),
        reinterpret_cast<void**>(dcompDevice.GetAddressOf()));
    if (FAILED(hr)) return hr;

    d3dDevice_ = std::move(d3dDevice);
    d3dImmediateContext_ = std::move(d3dImmediateContext);
    d2dFactory_ = std::move(d2dFactory);
    d2dDevice_ = std::move(d2dDevice);
    d2dContext_ = std::move(d2dContext);
    dcompDevice_ = std::move(dcompDevice);
    uiAnimationScheduler_.SetSoftwareRendering(usingWarp);
    return S_OK;
}

bool DesktopApp::InitGraphics()
{
    HRESULT hr = InitGraphicsDevices();
    if (FAILED(hr)) return false;

    // DWrite
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    if (FAILED(hr)) return false;
    RecreateItemTextFormat();
    RecreateComponentListTextFormat();

    dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &listItemTextFormat_);
    if (listItemTextFormat_)
    {
        listItemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        listItemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        listItemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &navTabTextFormat_);
    if (navTabTextFormat_)
    {
        navTabTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        navTabTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        navTabTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"",
        &fileCategoryTabTextFormat_);
    if (fileCategoryTabTextFormat_)
    {
        fileCategoryTabTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fileCategoryTabTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fileCategoryTabTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    faFontHandle_ = LoadFontAwesome();
    if (faFontHandle_)
    {
        faTextFormat_ = ComPtr<IDWriteTextFormat>(CreateFaTextFormat(dwriteFactory_.Get(), 14.0f));
        if (faTextFormat_)
        {
            faTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            faTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            faTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        // 菜单字体依赖弹出点所在显示器的 DPI，由
        // 自绘菜单在显示时按 DPI 创建临时 GDI 字体。
    }

    fluentIconFontHandle_ = LoadFluentSystemIconsRegular();
    if (fluentIconFontHandle_)
    {
        fluentIconTextFormat_.Attach(
            CreateFluentTextFormat(dwriteFactory_.Get(), 14.0f));
        if (fluentIconTextFormat_)
        {
            fluentIconTextFormat_->SetTextAlignment(
                DWRITE_TEXT_ALIGNMENT_CENTER);
            fluentIconTextFormat_->SetParagraphAlignment(
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fluentIconTextFormat_->SetWordWrapping(
                DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    return true;
}

void DesktopApp::RecreateItemTextFormat()
{
    if (!dwriteFactory_) return;
    itemTextLayoutCache_.clear();
    itemTextShadowCache_.clear();
    float fontSize = itemFontSizeCu_;
    float lineHeight = fontSize * 7.0f / 6.0f;
    float baseline = fontSize * 5.0f / 6.0f;
    dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, itemFontWeight_,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &itemTextFormat_);
    if (itemTextFormat_)
    {
        itemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        itemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        itemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        itemTextFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
            lineHeight, baseline);
    }
}

void DesktopApp::RecreateComponentListTextFormat()
{
    if (!dwriteFactory_) return;
    componentListTextLayoutCache_.clear();
    componentListTextShadowCache_.clear();
    componentListTextFormat_.Reset();
    const float lineHeight = listItemFontSizeCu_ * 7.0f / 6.0f;
    const float baseline = listItemFontSizeCu_ * 5.0f / 6.0f;
    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, itemFontWeight_,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        listItemFontSizeCu_, L"", &componentListTextFormat_);
    if (!componentListTextFormat_) return;
    componentListTextFormat_->SetTextAlignment(
        DWRITE_TEXT_ALIGNMENT_LEADING);
    componentListTextFormat_->SetParagraphAlignment(
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    componentListTextFormat_->SetWordWrapping(
        DWRITE_WORD_WRAPPING_NO_WRAP);
    componentListTextFormat_->SetLineSpacing(
        DWRITE_LINE_SPACING_METHOD_UNIFORM,
        lineHeight, baseline);
}

void DesktopApp::ResetCompositionRenderCaches()
{
    ResetDesktopWidgetComposition();
    ResetDesktopForegroundComposition();
    ResetDragPreviewCompositionResources();
    dragRenderCache_.Reset();
    ResetCollectionPopupAnimationCache();
    ResetLuaWidgetPanelAnimationCache();
    ResetPageNotifyAnimationCache();
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    acrylicNoiseBrushCache_.clear();
    privacyFileIconBitmap_.Reset();
    privacyFolderIconBitmap_.Reset();
    d2dIconCache_.clear();
    ResetDemoIconLoader();
    placeholderIconCache_.clear();
    quickNavSysIconCache_.clear();
    quickNavAppIconCache_.clear();
    shortcutArrowBitmap_.Reset();
    shortcutArrowBitmapSize_ = {};
    itemTextShadowCache_.clear();
    componentListTextShadowCache_.clear();
    itemTextEffectContext_.Reset();
}

void DesktopApp::RecoverCompositionRenderFailure(const wchar_t* stage, HRESULT hr)
{
    if (RequestGraphicsDeviceRecovery(stage, hr))
        return;
    wchar_t buf[192];
    wsprintfW(buf, L"%s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render", static_cast<unsigned>(hr));
    WriteDiagnosticLogEntry(buf);

    ResetCompositionRenderCaches();
    dcompSurface_.Reset();
    compositionWidth_ = 0;
    compositionHeight_ = 0;

    if (!compositionRenderRecoveryPending_ && hwnd_ && IsWindow(hwnd_))
    {
        compositionRenderRecoveryPending_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

HRESULT DesktopApp::CreateOrResizeCompositionSurface()
    {
        if (!dcompDevice_ || !dcompVisual_ || !hwnd_ || !IsWindow(hwnd_))
            return E_UNEXPECTED;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
        if (dcompSurface_ && compositionWidth_ == width && compositionHeight_ == height)
            return CreateOrResizeDesktopForegroundCompositionSurface();

        ComPtr<IDCompositionSurface> surface;
        HRESULT hr = dcompDevice_->CreateSurface(width, height,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"CreateSurface %ux%u FAILED hr=0x%08X", width, height, static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(buf);
            return hr;
        }
        hr = dcompVisual_->SetContent(surface.Get());
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"SetContent FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(buf);
            return hr;
        }
        hr = dcompDevice_->Commit();
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"CreateSurface Commit FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(buf);
            return hr;
        }

        dcompSurface_ = surface;
        compositionWidth_ = width;
        compositionHeight_ = height;
        return CreateOrResizeDesktopForegroundCompositionSurface();
    }

void DesktopApp::InitializeDockWindowTransition()
{
    dockWindowTransition_ =
        std::make_unique<DockWindowTransition>();
    if (!dockWindowTransition_->Initialize(
            instance_, &uiAnimationScheduler_,
            d2dDevice_.Get(), dcompDevice_.Get()))
        dockWindowTransition_.reset();
    if (dockWindowTransition_)
    {
        dockWindowTransition_->SetOcclusionRectsProvider([this] {
            return GetDockWindowTransitionOcclusionRects();
        });
        dockWindowTransition_->SetPresentationCallback([this](HWND) {
            ApplyFloatingDockLayerPolicy();
        });
        dockWindowTransition_->SetDiagnosticCallback([this](const wchar_t* message) {
            snowdesktop::dock_taskbar_diagnostics::Record(message,
                dockWindowTransition_->GetPresentationWindow());
            if (std::wcsncmp(message, L"Dock taskbar phase:", 19) == 0)
                return; // Buffered until the bounded observation ends.
            WriteDiagnosticLogEntry(message, DiagnosticLogLevel::Debug);
        });
    }

}

bool DesktopApp::RequestGraphicsDeviceRecovery(const wchar_t* stage, HRESULT hr)
{
    if (graphicsDeviceRecovery_.Pending()) return true;
    const HRESULT reason = d3dDevice_
        ? d3dDevice_->GetDeviceRemovedReason() : S_OK;
    if (!snowdesktop::GraphicsDeviceRecovery::IsDeviceFailure(hr, reason))
        return false;
    if (graphicsDeviceRecovery_.Request())
    {
        wchar_t message[256]{};
        swprintf_s(message,
            L"%s FAILED hr=0x%08X removedReason=0x%08X; queued graphics device recovery",
            stage ? stage : L"Graphics", static_cast<unsigned>(hr),
            static_cast<unsigned>(reason));
        WriteDiagnosticLogEntry(message);
    }
    return true;
}

void DesktopApp::ReleaseGraphicsDeviceResources()
{
    // Run only at the outer message-pump boundary, after every BeginDraw has
    // unwound. Keep all HWNDs, Lua instances, layout and user state intact.
    if (dockWindowTransition_)
    {
        dockWindowTransition_->SetPresentationCallback({});
        dockWindowTransition_->SetOcclusionRectsProvider({});
        dockWindowTransition_->SetDiagnosticCallback({});
    }
    dockWindowTransition_.reset();
    ResetCompositionRenderCaches();
    popupAnimationOverlay_ = {};
    luaWidgetPanelAnimationOverlay_ = {};
    pageNotifyAnimationOverlay_ = {};
    const auto releaseRoot = [](auto& target, auto& visual) {
        if (target) (void)target->SetRoot(nullptr);
        visual.Reset();
        target.Reset();
    };
    dcompSurface_.Reset();
    releaseRoot(dcompTarget_, dcompVisual_);
    compositionWidth_ = compositionHeight_ = 0;
    compositionCommitPending_ = false;
    compositionRenderRecoveryPending_ = false;
    for (const auto& host : persistentDockHosts_)
    {
        if (!host) continue;
        ResetFloatingDockCompositionResources(*host);
        releaseRoot(host->dcompTarget, host->dcompVisual);
        host->compositionRenderRecoveryPending = false;
    }
    ResetFloatingPopupCompositionResources();
    releaseRoot(floatingPopupDcompTarget_, floatingPopupDcompVisual_);
    floatingPopupCompositionRenderRecoveryPending_ = false;
    releaseRoot(dragPreviewDcompTarget_, dragPreviewDcompVisual_);
    ResetQuickNavCompositionResources();
    releaseRoot(quickNavDcompTarget_, quickNavDcompVisual_);
    quickNavDcompEffect_.Reset();
    quickNavDcompScaleTransform_.Reset();
    quickNavDcompDevice_.Reset();
    quickNavCompositionRenderRecoveryPending_ = false;
    if (widgetEngine_) widgetEngine_->ResetGraphicsResources(nullptr);
    if (d3dImmediateContext_) d3dImmediateContext_->ClearState();
}

void DesktopApp::ProcessGraphicsDeviceRecovery()
{
    if (!startupInitializationComplete_ || exitRequested_)
        return;
    if (!graphicsDeviceRecovery_.Pending())
    {
        // Also detect loss when all surfaces are idle or an inner renderer
        // returned a generic E_FAIL instead of propagating the DXGI HRESULT.
        (void)RequestGraphicsDeviceRecovery(L"Device health", S_OK);
        ComPtr<IDCompositionDevice> device;
        if (!graphicsDeviceRecovery_.Pending() && dcompDevice_ &&
            SUCCEEDED(dcompDevice_.As(&device)))
        {
            BOOL valid = TRUE;
            const HRESULT hr = device->CheckDeviceState(&valid);
            if (FAILED(hr)) (void)RequestGraphicsDeviceRecovery(L"CheckDeviceState", hr);
            else if (!valid)
                (void)RequestGraphicsDeviceRecovery(L"CheckDeviceState", DXGI_ERROR_DEVICE_REMOVED);
        }
    }
    const bool drawing = compositionPaintInProgress_ ||
        desktopWidgetCompositionDrawInProgress_ ||
        dragPreviewCompositionPaintInProgress_ ||
        floatingPopupCompositionPaintInProgress_ ||
        quickNavCompositionPaintInProgress_ || IsAnyPersistentDockHostPainting();
    if (!graphicsDeviceRecovery_.Ready(GetTickCount64(), drawing)) return;

    ReleaseGraphicsDeviceResources();
    HRESULT hr = InitGraphicsDevices();
    // Explorer recovery may temporarily remove the desktop HWND. Rebuild the
    // device even then: CreateDesktopOverlayWindow otherwise keeps retrying
    // target creation on the lost device and can never restore that HWND.
    if (SUCCEEDED(hr) && hwnd_ && IsWindow(hwnd_))
    {
        hr = dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_);
        if (SUCCEEDED(hr)) hr = dcompDevice_->CreateVisual(&dcompVisual_);
        if (SUCCEEDED(hr)) hr = dcompTarget_->SetRoot(dcompVisual_.Get());
        if (SUCCEEDED(hr)) hr = CreateOrResizeCompositionSurface();
    }
    graphicsDeviceRecovery_.Complete(GetTickCount64(), SUCCEEDED(hr));
    if (FAILED(hr))
    {
        wchar_t message[192]{};
        swprintf_s(message, L"Graphics device recovery FAILED hr=0x%08X; retry in 2000 ms",
            static_cast<unsigned>(hr));
        WriteDiagnosticLogEntry(message);
        return;
    }
    if (widgetEngine_) widgetEngine_->ResetGraphicsResources(d2dContext_.Get());
    InitializeDockWindowTransition();
    desktopBackdropFullCollectionPending_ = true;
    const auto repaint = [](HWND window) {
        if (window && IsWindow(window)) InvalidateRect(window, nullptr, FALSE);
    };
    repaint(hwnd_);
    for (const auto& host : persistentDockHosts_)
        if (host) repaint(host->hwnd);
    repaint(floatingPopupHwnd_);
    repaint(quickNavigationHwnd_);
    repaint(dragPreviewHwnd_);
    EnsureUiAnimationFrame();
    WriteDiagnosticLogEntry(L"Graphics devices recreated; surface repaints requested");
}
