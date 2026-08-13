#include "app.h"

// Graphics-device and composition-surface lifecycle.

bool DesktopApp::InitGraphics()
{
    // D3D11
    D3D_FEATURE_LEVEL fl{};
    bool usingWarp = false;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice_, &fl, nullptr);
    if (FAILED(hr))
    {
        usingWarp = true;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3dDevice_, &fl, nullptr);
    }
    {
        wchar_t buf[128];
        wsprintfW(buf, L"D3D11 driver=%s hr=0x%08X feature=0x%04X",
            usingWarp ? L"WARP" : L"HARDWARE",
            static_cast<unsigned>(hr), static_cast<unsigned>(fl));
        WriteDiagnosticLogEntry(buf);
    }
    if (FAILED(hr)) return false;
    uiAnimationScheduler_.SetSoftwareRendering(usingWarp);
    d3dDevice_->GetImmediateContext(&d3dImmediateContext_);
    if (!d3dImmediateContext_) return false;

    // D2D
    D2D1_FACTORY_OPTIONS factoryOptions{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &factoryOptions,
        reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice_.As(&dxgiDevice);
    if (FAILED(hr)) return false;
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
    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (FAILED(hr)) return false;
    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (FAILED(hr)) return false;

    // DComp — create from the D2D device for interop
    hr = DCompositionCreateDevice2(d2dDevice_.Get(), __uuidof(IDCompositionDesktopDevice),
        reinterpret_cast<void**>(dcompDevice_.GetAddressOf()));
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
    float fontSize = itemFontSize_;
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
    const float lineHeight = listItemFontSize_ * 7.0f / 6.0f;
    const float baseline = listItemFontSize_ * 5.0f / 6.0f;
    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, itemFontWeight_,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        listItemFontSize_, L"", &componentListTextFormat_);
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
        RECT client{};
        GetClientRect(hwnd_, &client);
        const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
        if (dcompSurface_ && compositionWidth_ == width && compositionHeight_ == height)
            return S_OK;

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
        return S_OK;
    }
