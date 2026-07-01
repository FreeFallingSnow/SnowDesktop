#pragma once
// Inline implementations for DesktopApp — Graphics & Rendering.
// This file is included by app_oo.h after the class definition.

// ── Quick Navigation Theme Colors ───────────────────────────

struct QuickNavTheme {
    COLORREF windowBg, windowBorder;
    COLORREF searchBg, searchBorder, searchEditBg;
    COLORREF tabActiveFill, tabActiveStroke;
    COLORREF tabHoverFill, tabHoverStroke;
    COLORREF tabDefaultFill, tabDefaultStroke;
    COLORREF tabText, tabSeparator;
    COLORREF tabDragFill, tabDragStroke;
    COLORREF tabDragFloatFill, tabDragFloatStroke, tabDragFloatText;
    COLORREF tabDragIndicator;
    COLORREF itemHoverFill, itemHoverStroke;
    COLORREF itemText;
    COLORREF headerText, headerSeparator;
    COLORREF appRowHoverFill, appRowHoverStroke;
    COLORREF appNameText, appTypeText;
    COLORREF expandHoverText, expandDefaultText;
    COLORREF emptyText, emptyHeaderText;
    COLORREF scrollTrack, scrollThumbDefault, scrollThumbHover;

    D2D1_COLOR_F popupBg, popupBorder, popupTitle;
    D2D1_COLOR_F iconHoverBgFill, iconHoverBgStroke;
    D2D1_COLOR_F iconSelectBgFill, iconSelectBgStroke;
    D2D1_COLOR_F iconTextColor;
    D2D1_COLOR_F iconShadowFallback;
};

inline const QuickNavTheme kQuickNavDark = {
    // GDI
    RGB(18, 22, 30),    // windowBg
    RGB(120, 130, 150),  // windowBorder
    RGB(255, 255, 255),  // searchBg
    RGB(92, 105, 128),   // searchBorder
    RGB(18, 22, 30),     // searchEditBg
    RGB(48, 112, 215),   // tabActiveFill
    RGB(82, 140, 235),   // tabActiveStroke
    RGB(66, 72, 84),     // tabHoverFill
    RGB(68, 76, 92),     // tabHoverStroke
    RGB(42, 47, 58),     // tabDefaultFill
    RGB(68, 76, 92),     // tabDefaultStroke
    RGB(245, 248, 252),  // tabText
    RGB(90, 96, 110),    // tabSeparator
    RGB(80, 92, 112),    // tabDragFill
    RGB(120, 140, 180),  // tabDragStroke
    RGB(60, 80, 110),    // tabDragFloatFill
    RGB(100, 130, 200),  // tabDragFloatStroke
    RGB(245, 248, 252),  // tabDragFloatText
    RGB(82, 140, 235),   // tabDragIndicator
    RGB(58, 68, 86),     // itemHoverFill
    RGB(78, 92, 118),    // itemHoverStroke
    RGB(245, 248, 252),  // itemText
    RGB(182, 194, 212),  // headerText
    RGB(48, 56, 70),     // headerSeparator
    RGB(46, 56, 72),     // appRowHoverFill
    RGB(68, 82, 106),    // appRowHoverStroke
    RGB(245, 248, 252),  // appNameText
    RGB(146, 156, 174),  // appTypeText
    RGB(226, 236, 252),  // expandHoverText
    RGB(170, 184, 208),  // expandDefaultText
    RGB(160, 168, 182),  // emptyText
    RGB(182, 194, 212),  // emptyHeaderText
    RGB(55, 62, 76),     // scrollTrack
    RGB(136, 146, 166),  // scrollThumbDefault
    RGB(167, 178, 199),  // scrollThumbHover
    // D2D
    {0.08f, 0.10f, 0.13f, 1.0f},     // popupBg
    {1.0f, 1.0f, 1.0f, 0.50f},       // popupBorder
    {1.0f, 1.0f, 1.0f, 1.0f},        // popupTitle
    {1.0f, 1.0f, 1.0f, 0.08f},       // iconHoverBgFill
    {1.0f, 1.0f, 1.0f, 0.20f},       // iconHoverBgStroke
    {0.55f, 0.55f, 0.55f, 0.34f},    // iconSelectBgFill
    {0.78f, 0.78f, 0.78f, 0.55f},    // iconSelectBgStroke
    {1.0f, 1.0f, 1.0f, 1.0f},        // iconTextColor
    {0.0f, 0.0f, 0.0f, 0.80f},       // iconShadowFallback
};

inline const QuickNavTheme kQuickNavLight = {
    // GDI
    RGB(246, 248, 252),  // windowBg
    RGB(180, 190, 200),  // windowBorder
    RGB(255, 255, 255),  // searchBg
    RGB(170, 182, 198),  // searchBorder
    RGB(255, 255, 255),  // searchEditBg
    RGB(48, 112, 215),   // tabActiveFill
    RGB(82, 140, 235),   // tabActiveStroke
    RGB(215, 222, 236),  // tabHoverFill
    RGB(190, 200, 215),  // tabHoverStroke
    RGB(230, 234, 242),  // tabDefaultFill
    RGB(190, 200, 215),  // tabDefaultStroke
    RGB(28, 34, 44),     // tabText
    RGB(200, 208, 220),  // tabSeparator
    RGB(195, 205, 220),  // tabDragFill
    RGB(140, 160, 190),  // tabDragStroke
    RGB(180, 195, 215),  // tabDragFloatFill
    RGB(120, 150, 200),  // tabDragFloatStroke
    RGB(28, 34, 44),     // tabDragFloatText
    RGB(82, 140, 235),   // tabDragIndicator
    RGB(220, 228, 240),  // itemHoverFill
    RGB(190, 200, 218),  // itemHoverStroke
    RGB(28, 34, 44),     // itemText
    RGB(110, 120, 138),  // headerText
    RGB(210, 218, 228),  // headerSeparator
    RGB(222, 228, 242),  // appRowHoverFill
    RGB(190, 200, 218),  // appRowHoverStroke
    RGB(28, 34, 44),     // appNameText
    RGB(125, 135, 152),  // appTypeText
    RGB(48, 112, 215),   // expandHoverText
    RGB(100, 112, 135),  // expandDefaultText
    RGB(135, 145, 162),  // emptyText
    RGB(110, 120, 138),  // emptyHeaderText
    RGB(215, 222, 232),  // scrollTrack
    RGB(168, 178, 198),  // scrollThumbDefault
    RGB(140, 155, 175),  // scrollThumbHover
    // D2D
    {0.96f, 0.97f, 0.98f, 1.0f},     // popupBg
    {0.50f, 0.55f, 0.60f, 0.50f},    // popupBorder
    {0.10f, 0.12f, 0.16f, 1.0f},     // popupTitle
    {0.0f, 0.0f, 0.0f, 0.06f},       // iconHoverBgFill
    {0.0f, 0.0f, 0.0f, 0.12f},       // iconHoverBgStroke
    {0.20f, 0.40f, 0.70f, 0.18f},    // iconSelectBgFill
    {0.25f, 0.50f, 0.80f, 0.40f},    // iconSelectBgStroke
    {0.10f, 0.12f, 0.16f, 1.0f},     // iconTextColor
    {0.0f, 0.0f, 0.0f, 0.12f},       // iconShadowFallback
};

// ── Graphics ─────────────────────────────────────────────────

inline void DragRenderCache::Reset()
{
    bitmap_.Reset();
    width_ = 0;
    height_ = 0;
    revision_ = 0;
    if (context_)
        context_->SetTarget(nullptr);
}

inline bool DragRenderCache::Ensure(ID2D1Device* device, D2D1_SIZE_U pixelSize,
    std::uint64_t revision, const std::function<void(ID2D1DeviceContext*)>& drawStatic)
{
    if (!device || pixelSize.width == 0 || pixelSize.height == 0 || !drawStatic)
        return false;

    if (bitmap_ && width_ == pixelSize.width && height_ == pixelSize.height &&
        revision_ == revision)
        return true;

    if (!context_)
    {
        HRESULT hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_);
        if (FAILED(hr) || !context_)
            return false;
    }

    bitmap_.Reset();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = context_->CreateBitmap(pixelSize, nullptr, 0, &bp, &bitmap_);
    if (FAILED(hr) || !bitmap_)
        return false;

    context_->SetTarget(bitmap_.Get());
    context_->SetDpi(96.0f, 96.0f);
    context_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context_->BeginDraw();
    context_->Clear(D2D1::ColorF(0, 0, 0, 0));
    drawStatic(context_.Get());
    hr = context_->EndDraw();
    context_->SetTarget(nullptr);
    if (FAILED(hr))
    {
        bitmap_.Reset();
        return false;
    }

    width_ = pixelSize.width;
    height_ = pixelSize.height;
    revision_ = revision;
    return true;
}

inline void DragRenderCache::Draw(ID2D1DeviceContext* ctx) const
{
    if (!ctx || !bitmap_) return;
    D2D1_SIZE_F sz = bitmap_->GetSize();
    ctx->DrawBitmap(bitmap_.Get(), D2D1::RectF(0, 0, sz.width, sz.height),
        1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

inline bool DesktopApp::InitGraphics()
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
        WriteCrashLogEntry(buf);
    }
    if (FAILED(hr)) return false;

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
            WriteCrashLogEntry(buf);
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

        const int menuHeight = -std::max(12, GetSystemMetrics(SM_CYMENUCHECK) * 8 / 10);
        faMenuFont_ = CreateFontW(menuHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Font Awesome 6 Free Solid");
    }

    return true;
}

inline void DesktopApp::RecreateItemTextFormat()
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

inline void DesktopApp::ResetCompositionRenderCaches()
{
    dragRenderCache_.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    d2dIconCache_.clear();
    placeholderIconCache_.clear();
    shortcutArrowBitmap_.Reset();
    shortcutArrowBitmapSize_ = {};
    itemTextShadowCache_.clear();
    itemTextEffectContext_.Reset();
}

inline void DesktopApp::RecoverCompositionRenderFailure(const wchar_t* stage, HRESULT hr)
{
    wchar_t buf[192];
    wsprintfW(buf, L"%s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render", static_cast<unsigned>(hr));
    WriteCrashLogEntry(buf);

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

inline HRESULT DesktopApp::CreateOrResizeCompositionSurface()
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
            WriteCrashLogEntry(buf);
            return hr;
        }
        hr = dcompVisual_->SetContent(surface.Get());
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"SetContent FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteCrashLogEntry(buf);
            return hr;
        }
        hr = dcompDevice_->Commit();
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"CreateSurface Commit FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteCrashLogEntry(buf);
            return hr;
        }

        dcompSurface_ = surface;
        compositionWidth_ = width;
        compositionHeight_ = height;
        return S_OK;
    }

inline void DesktopApp::OnPaint()
    {
        HRESULT hr = CreateOrResizeCompositionSurface();
        if (FAILED(hr))
        {
            RecoverCompositionRenderFailure(L"CreateOrResizeCompositionSurface", hr);
            return;
        }

        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        hr = dcompSurface_->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
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
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x), static_cast<float>(updateOffset.y)));
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        if (!desktopIconsHidden_)
            RenderFrame(context.Get());
        else if (showHiddenHint_)
            DrawHiddenHintOverlay(context.Get());

        if (showWidgetAddedHint_)
            DrawWidgetAddedHintOverlay(context.Get());

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();

        hr = dcompSurface_->EndDraw();
        if (FAILED(hr))
        {
            RecoverCompositionRenderFailure(L"EndDraw", hr);
            return;
        }

        hr = dcompDevice_->Commit();
        if (FAILED(hr))
        {
            RecoverCompositionRenderFailure(L"Paint Commit", hr);
            return;
        }
        compositionRenderRecoveryPending_ = false;
    }

inline D2D1_RECT_F DesktopApp::ToD2DRect(const RECT& r)
{
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
        static_cast<float>(r.right), static_cast<float>(r.bottom));
}

inline std::uint64_t D2DColorBrushKey(const D2D1_COLOR_F& c)
{
    const auto quantize = [](float value) -> std::uint64_t {
        return static_cast<std::uint64_t>(std::lround(
            std::clamp(value, 0.0f, 1.0f) * 65535.0f));
    };
    return quantize(c.r) |
        (quantize(c.g) << 16) |
        (quantize(c.b) << 32) |
        (quantize(c.a) << 48);
}

/** @brief 将 GDI COLORREF（0x00BBGGRR）转换为 D2D1_COLOR_F，可选 alpha。 */
inline D2D1_COLOR_F ToD2DColor(COLORREF c, float a = 1.0f)
{
    // COLORREF 是 0x00BBGGRR；D2D1::ColorF(UINT32) 期望 0xRRGGBB，故显式按通道构造。
    return D2D1::ColorF(
        GetRValue(c) / 255.0f,
        GetGValue(c) / 255.0f,
        GetBValue(c) / 255.0f,
        a);
}

/** @brief 用 D2D 绘制一条 1 像素粗的水平/垂直分隔线。 */
inline void DesktopApp::DrawD2DSeparator(ID2D1RenderTarget* ctx, RECT rect, const D2D1_COLOR_F& color)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const std::uint64_t key = D2DColorBrushKey(color);
    auto it = brushCache_.find(key);
    if (it == brushCache_.end())
    {
        ComPtr<ID2D1SolidColorBrush> b;
        if (FAILED(ctx->CreateSolidColorBrush(color, &b)) || !b) return;
        it = brushCache_.emplace(key, std::move(b)).first;
    }
    if (it != brushCache_.end() && it->second)
        ctx->FillRectangle(ToD2DRect(rect), it->second.Get());
}

inline float DesktopApp::GetItemLayoutScale(RECT bounds) const
{
    const POINT center = {
        bounds.left + (bounds.right - bounds.left) / 2,
        bounds.top + (bounds.bottom - bounds.top) / 2
    };
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, center))
        {
            // cellWidth/cellHeight exclude the inter-cell gap. Use the full
            // grid pitch so content is not shrunk a second time after layout
            // has already reserved spacing between cells.
            const int pitchX = page.cellWidth +
                (page.columns > 1 ? page.gapX : 0);
            const int pitchY = page.cellHeight +
                (page.rows > 1 ? page.gapY : 0);
            return std::max(0.1f, std::min(
                static_cast<float>(std::max(1, pitchX)) /
                    static_cast<float>(kCellWidth),
                static_cast<float>(std::max(1, pitchY)) /
                    static_cast<float>(kMinCellHeight)));
        }
    }
    return 1.0f;
}

inline RECT DesktopApp::GetItemIconRect(RECT bounds) const
{
    const float layoutScale = GetItemLayoutScale(bounds);
    const int inset = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    const int topInset = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    const float lineHeight = itemFontSize_ * 7.0f / 6.0f * layoutScale;
    const int textHeight = std::max(1, static_cast<int>(std::floor(lineHeight * 2.0f)) - 1);
    const int cellW = bounds.right - bounds.left;
    const int cellH = bounds.bottom - bounds.top;
    if (cellH < static_cast<int>(std::round(50.0f * layoutScale)))
    {
        const int iconSz = std::max(1, std::min({
            static_cast<int>(std::round(32.0f * layoutScale)),
            std::max(1, cellW - inset * 2),
            std::max(1, cellH - inset * 2) }));
        return MakeRect(
            bounds.left + inset,
            bounds.top + (cellH - iconSz) / 2,
            bounds.left + inset + iconSz,
            bounds.top + (cellH + iconSz) / 2);
    }
    const int maxIconW = std::max(1, cellW - inset * 2);
    const int maxIconH = std::max(1, cellH - textHeight - inset * 2);
    // The title owns the bottom text band. Let the icon fill the remaining
    // square area instead of capping it at the old fixed 64-pixel size.
    const int iconSz = std::max(1, std::min(maxIconW, maxIconH));
    const int iconX = bounds.left + (cellW - iconSz) / 2;
    const int iconY = bounds.top + topInset;
    return MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
}

inline RECT DesktopApp::GetQuickNavItemIconRect(RECT bounds) const
{
    const int cellW = std::max<LONG>(1, bounds.right - bounds.left);
    const int cellH = std::max<LONG>(1, bounds.bottom - bounds.top);
    const int inset = std::max(1, QuickNavScale(2));
    const int titleBandH = std::max(1, QuickNavScale(kQuickNavigationTextHeight));
    const int titleGap = std::max(1, QuickNavScale(2));
    const int maxIconW = std::max(1, cellW - inset * 2);
    const int maxIconH = std::max(1, cellH - titleBandH - titleGap - inset);
    const int iconSz = std::max(1, std::min({
        QuickNavScale(48),
        maxIconW,
        maxIconH
    }));
    const int iconX = bounds.left + (cellW - iconSz) / 2;
    const int iconY = bounds.top + inset;
    return MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
}

inline RECT DesktopApp::GetItemTextRect(RECT bounds, bool expanded) const
{
    const float layoutScale = GetItemLayoutScale(bounds);
    RECT iconRect = GetItemIconRect(bounds);
    const int inset = std::max(1, static_cast<int>(std::round(4.0f * layoutScale)));
    const int textTop = iconRect.bottom +
        std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    const float lineHeight = itemFontSize_ * 7.0f / 6.0f * layoutScale;
    const int textH = expanded
        ? std::max(
            static_cast<int>(std::round(kTextExpandedHeight * layoutScale)),
            static_cast<int>(std::ceil(lineHeight * 3.0f)))
        : std::max(1, static_cast<int>(std::floor(lineHeight * 2.0f)) - 1);

    // The collapsed label is clipped just before a third line can begin.
    // Selected labels intentionally extend below the cell to reveal the lines
    // hidden in the normal two-line state.
    return MakeRect(bounds.left + inset, textTop,
        bounds.right - inset, textTop + textH);
}

inline RECT DesktopApp::GetItemSelectionRect(RECT bounds, bool expanded) const
{
    const float layoutScale = GetItemLayoutScale(bounds);
    RECT textRect = GetItemTextRect(bounds, expanded);
    RECT selection = UnionCopy(GetItemIconRect(bounds), textRect);
    const int sideInset = std::max(1, static_cast<int>(std::round(3.0f * layoutScale)));
    const int horizontalPad = std::max(1, static_cast<int>(std::round(4.0f * layoutScale)));
    const int verticalPad = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    selection.left = std::max(bounds.left + sideInset, selection.left - horizontalPad);
    selection.top = std::max(bounds.top, selection.top - verticalPad);
    selection.right = std::min(bounds.right - sideInset, selection.right + horizontalPad);
    selection.bottom = std::min(bounds.bottom - verticalPad, textRect.bottom);
    return selection;
}

inline void DesktopApp::DrawD2DRoundedRectangle(ID2D1RenderTarget* ctx, RECT rect, float radius,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }

    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(ToD2DRect(rect), radius, radius);
    if (fill.a > 0.0f)
    {
        std::uint64_t k = D2DColorBrushKey(fill);
        auto it = brushCache_.find(k);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> b;
            if (SUCCEEDED(ctx->CreateSolidColorBrush(fill, &b)) && b)
                it = brushCache_.emplace(k, std::move(b)).first;
            else
                return;
        }
        if (it != brushCache_.end() && it->second)
            ctx->FillRoundedRectangle(rounded, it->second.Get());
    }
    if (stroke.a > 0.0f)
    {
        std::uint64_t k = D2DColorBrushKey(stroke);
        auto it = brushCache_.find(k);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> b;
            if (SUCCEEDED(ctx->CreateSolidColorBrush(stroke, &b)) && b)
                it = brushCache_.emplace(k, std::move(b)).first;
            else
                return;
        }
        if (it != brushCache_.end() && it->second)
            ctx->DrawRoundedRectangle(rounded, it->second.Get(), strokeWidth, nullptr);
    }
}

inline void DesktopApp::DrawD2DFilledRectangle(ID2D1RenderTarget* ctx, RECT rect,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }

    if (fill.a > 0.0f)
    {
        std::uint64_t k = D2DColorBrushKey(fill);
        auto it = brushCache_.find(k);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> b;
            if (SUCCEEDED(ctx->CreateSolidColorBrush(fill, &b)) && b)
                it = brushCache_.emplace(k, std::move(b)).first;
        }
        if (it != brushCache_.end() && it->second)
            ctx->FillRectangle(ToD2DRect(rect), it->second.Get());
    }
    if (stroke.a > 0.0f)
    {
        std::uint64_t k = D2DColorBrushKey(stroke);
        auto it = brushCache_.find(k);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> b;
            if (SUCCEEDED(ctx->CreateSolidColorBrush(stroke, &b)) && b)
                it = brushCache_.emplace(k, std::move(b)).first;
        }
        if (it != brushCache_.end() && it->second)
            ctx->DrawRectangle(ToD2DRect(rect), it->second.Get(), 1.0f, nullptr);
    }
}

inline void DesktopApp::DrawStyledItemTextLayout(ID2D1RenderTarget* context,
    IDWriteTextLayout* layout, const std::wstring& shadowKey,
    D2D1_POINT_2F origin, D2D1_SIZE_F layoutSize,
    float layoutScale, float opacity, bool lightTheme)
{
    if (!context || !layout || shadowKey.empty()) return;
    if (context != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = context;
    }

    auto getBrush = [&](const D2D1_COLOR_F& color) -> ID2D1SolidColorBrush* {
        const std::uint64_t key = D2DColorBrushKey(color);
        auto it = brushCache_.find(key);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(context->CreateSolidColorBrush(color, &brush)) || !brush)
                return nullptr;
            it = brushCache_.emplace(key, std::move(brush)).first;
        }
        return it->second.Get();
    };
    ID2D1SolidColorBrush* textBrush =
        getBrush(lightTheme
            ? D2D1::ColorF(0.10f, 0.12f, 0.16f, opacity)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity));

    const float tw = std::max(1.0f, layoutSize.width);
    const float th = std::max(1.0f, layoutSize.height);
    const float shadowScale = std::max(0.5f, layoutScale);
    ComPtr<ID2D1DeviceContext> deviceContext;
    const bool supportsEffects =
        SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&deviceContext))) && deviceContext;

    if (supportsEffects && !itemTextEffectContext_ && d2dDevice_)
    {
        d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &itemTextEffectContext_);
        if (itemTextEffectContext_)
        {
            itemTextEffectContext_->SetDpi(96.0f, 96.0f);
            itemTextEffectContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        }
    }

    // Render both shadow layers through Direct2D's continuous Gaussian shadow
    // effect. Cache the result per layout so normal desktop repaints only need
    // one bitmap draw per label.
    if (supportsEffects && itemTextEffectContext_ && !lightTheme)
    {
        auto shadowIt = itemTextShadowCache_.find(shadowKey);
        if (shadowIt == itemTextShadowCache_.end())
        {
            const UINT shadowPadding =
                static_cast<UINT>(std::max(1.0f, std::ceil(6.0f * shadowScale)));
            const UINT shadowWidth = static_cast<UINT>(
                std::ceil(tw + shadowPadding * 2.0f + shadowScale));
            const UINT shadowHeight = static_cast<UINT>(
                std::ceil(th + shadowPadding * 2.0f + shadowScale));

            ComPtr<ID2D1CommandList> shadowMask;
            ComPtr<ID2D1Bitmap1> shadowBitmap;
            D2D1_BITMAP_PROPERTIES1 shadowBitmapProperties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(itemTextEffectContext_->CreateCommandList(&shadowMask)) &&
                shadowMask &&
                SUCCEEDED(itemTextEffectContext_->CreateBitmap(
                    D2D1::SizeU(shadowWidth, shadowHeight), nullptr, 0,
                    &shadowBitmapProperties, &shadowBitmap)) &&
                shadowBitmap)
            {
                itemTextEffectContext_->SetTarget(shadowMask.Get());
                itemTextEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
                itemTextEffectContext_->BeginDraw();

                ComPtr<ID2D1SolidColorBrush> maskBrush;
                if (SUCCEEDED(itemTextEffectContext_->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &maskBrush)) && maskBrush)
                {
                    itemTextEffectContext_->DrawTextLayout(
                        D2D1::Point2F(
                            static_cast<float>(shadowPadding),
                            static_cast<float>(shadowPadding)),
                        layout, maskBrush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }

                HRESULT maskHr = itemTextEffectContext_->EndDraw();
                itemTextEffectContext_->SetTarget(nullptr);
                if (SUCCEEDED(maskHr) && SUCCEEDED(shadowMask->Close()))
                {
                    ComPtr<ID2D1Effect> softShadow;
                    ComPtr<ID2D1Effect> offsetShadow;
                    ComPtr<ID2D1Effect> offsetTransform;
                    if (SUCCEEDED(itemTextEffectContext_->CreateEffect(
                        CLSID_D2D1Shadow, &softShadow)) && softShadow &&
                        SUCCEEDED(itemTextEffectContext_->CreateEffect(
                            CLSID_D2D1Shadow, &offsetShadow)) && offsetShadow &&
                        SUCCEEDED(itemTextEffectContext_->CreateEffect(
                            CLSID_D2D12DAffineTransform, &offsetTransform)) &&
                        offsetTransform)
                    {
                        softShadow->SetInput(0, shadowMask.Get());
                        softShadow->SetValue(
                            D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                            1.5f * shadowScale);
                        softShadow->SetValue(
                            D2D1_SHADOW_PROP_COLOR,
                            D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f, 0.95f });
                        softShadow->SetValue(
                            D2D1_SHADOW_PROP_OPTIMIZATION,
                            D2D1_SHADOW_OPTIMIZATION_QUALITY);

                        offsetShadow->SetInput(0, shadowMask.Get());
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                            0.5f * shadowScale);
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_COLOR,
                            D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f, 1.0f });
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_OPTIMIZATION,
                            D2D1_SHADOW_OPTIMIZATION_QUALITY);

                        offsetTransform->SetInputEffect(0, offsetShadow.Get());
                        offsetTransform->SetValue(
                            D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                            D2D1::Matrix3x2F::Translation(shadowScale, shadowScale));
                        offsetTransform->SetValue(
                            D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                            D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);

                        itemTextEffectContext_->SetTarget(shadowBitmap.Get());
                        itemTextEffectContext_->BeginDraw();
                        itemTextEffectContext_->Clear(
                            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                        itemTextEffectContext_->DrawImage(softShadow.Get());
                        itemTextEffectContext_->DrawImage(softShadow.Get());
                        itemTextEffectContext_->DrawImage(offsetTransform.Get());
                        HRESULT shadowHr = itemTextEffectContext_->EndDraw();
                        itemTextEffectContext_->SetTarget(nullptr);
                        if (SUCCEEDED(shadowHr))
                        {
                            shadowIt = itemTextShadowCache_.emplace(
                                shadowKey, std::move(shadowBitmap)).first;
                        }
                    }
                }
            }
        }

        if (shadowIt != itemTextShadowCache_.end() && shadowIt->second)
        {
            const float shadowPadding =
                std::max(1.0f, std::ceil(6.0f * shadowScale));
            const D2D1_SIZE_F shadowSize = shadowIt->second->GetSize();
            context->DrawBitmap(
                shadowIt->second.Get(),
                D2D1::RectF(
                    origin.x - shadowPadding,
                    origin.y - shadowPadding,
                    origin.x - shadowPadding + shadowSize.width,
                    origin.y - shadowPadding + shadowSize.height),
                opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    else
    {
        ID2D1SolidColorBrush* shadowBrush =
            getBrush(lightTheme
                ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f * opacity)
                : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.80f * opacity));
        if (shadowBrush)
        {
            context->DrawTextLayout(
                D2D1::Point2F(origin.x + layoutScale, origin.y + layoutScale),
                layout, shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            context->DrawTextLayout(
                D2D1::Point2F(origin.x, origin.y + layoutScale),
                layout, shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (textBrush)
        context->DrawTextLayout(origin, layout, textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline void DesktopApp::DrawItemText(ID2D1RenderTarget* context, RECT bounds,
    const std::wstring& text, bool selected, float opacity, bool lightTheme)
{
    if (!dwriteFactory_ || !itemTextFormat_ || text.empty()) return;

    RECT textRect = GetItemTextRect(bounds, selected);
    float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
    float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));

    const float layoutScale = GetItemLayoutScale(bounds);
    const int scaleKey = static_cast<int>(std::round(layoutScale * 1000.0f));
    std::wstring layoutKey = L"grid\x1f" + text + L"\x1f" +
        std::to_wstring(textRect.right - textRect.left) + L"x" +
        std::to_wstring(textRect.bottom - textRect.top) + L"@" +
        std::to_wstring(scaleKey);
    auto layoutIt = itemTextLayoutCache_.find(layoutKey);
    if (layoutIt == itemTextLayoutCache_.end())
    {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()),
            itemTextFormat_.Get(), tw, th, &layout)) || !layout)
            return;
        const DWRITE_TEXT_RANGE fullRange{ 0, static_cast<UINT32>(text.size()) };
        layout->SetFontSize(itemFontSize_ * layoutScale, fullRange);
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
            itemFontSize_ * 7.0f / 6.0f * layoutScale,
            itemFontSize_ * 5.0f / 6.0f * layoutScale);
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        bool isSingleLine = (metrics.lineCount == 1);
        if (!isSingleLine)
        {
            ComPtr<IDWriteTextLayout> measureLayout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                itemTextFormat_.Get(), 10000.0f, 10000.0f, &measureLayout)) && measureLayout)
            {
                measureLayout->SetFontSize(itemFontSize_ * layoutScale, fullRange);
                DWRITE_TEXT_METRICS m{};
                measureLayout->GetMetrics(&m);
                isSingleLine = (m.widthIncludingTrailingWhitespace <= tw + 2.0f);
            }
        }
        if (isSingleLine)
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layoutIt = itemTextLayoutCache_.emplace(std::move(layoutKey), std::move(layout)).first;
    }

    float ty = static_cast<float>(textRect.top);
    DWRITE_TEXT_METRICS metrics{};
    layoutIt->second->GetMetrics(&metrics);
    bool isSingleLine = (metrics.lineCount == 1);
    if (!isSingleLine)
    {
        ComPtr<IDWriteTextLayout> measureLayout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()),
            itemTextFormat_.Get(), 10000.0f, 10000.0f, &measureLayout)) && measureLayout)
        {
            const DWRITE_TEXT_RANGE fullRange{
                0, static_cast<UINT32>(text.size()) };
            measureLayout->SetFontSize(itemFontSize_ * layoutScale, fullRange);
            DWRITE_TEXT_METRICS m{};
            measureLayout->GetMetrics(&m);
            isSingleLine = (m.widthIncludingTrailingWhitespace <= tw + 2.0f);
        }
    }
    if (isSingleLine && selected)
    {
        RECT cr = GetItemTextRect(bounds, false);
        float collapsedH = static_cast<float>(cr.bottom - cr.top);
        ty = cr.top + (collapsedH - th) * 0.5f;
    }

    DrawStyledItemTextLayout(
        context, layoutIt->second.Get(), layoutIt->first,
        D2D1::Point2F(static_cast<float>(textRect.left), ty),
        D2D1::SizeF(tw, th), layoutScale, opacity, lightTheme);
}

inline void DesktopApp::DrawQuickNavItemText(ID2D1RenderTarget* ctx, RECT bounds,
    const std::wstring& text, bool /*selected*/, bool lightTheme)
{
    if (!ctx || !dwriteFactory_ || !quickNavItemTextFormat_ || text.empty())
        return;

    const float fontSize = quickNavItemTextFormat_->GetFontSize();
    const float lineSpacing = std::max(1.0f, std::floor(fontSize * 1.08f));
    const float baseline = std::max(1.0f, std::floor(fontSize * 0.84f));
    const int textHeight = std::max(1, static_cast<int>(std::ceil(lineSpacing * 2.0f)));
    RECT iconRect = GetQuickNavItemIconRect(bounds);
    const int horizontalPad = QuickNavScale(4);
    const int topGap = std::max(1, QuickNavScale(2));
    const int textTop = std::max<LONG>(bounds.top, iconRect.bottom + topGap);
    RECT textRect = MakeRect(
        bounds.left + horizontalPad,
        textTop,
        bounds.right - horizontalPad,
        std::min<LONG>(bounds.bottom, textTop + textHeight));
    if (IsRectEmptyRect(textRect))
        return;

    const float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
    const float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(),
        static_cast<UINT32>(text.size()), quickNavItemTextFormat_.Get(),
        tw, th, &layout)) || !layout)
        return;

    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineSpacing, baseline);
    DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    ComPtr<IDWriteInlineObject> trimmingSign;
    if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(
        quickNavItemTextFormat_.Get(), &trimmingSign)) && trimmingSign)
        layout->SetTrimming(&trimming, trimmingSign.Get());

    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.lineCount == 1)
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    auto getBrush = [&](const D2D1_COLOR_F& color) -> ID2D1SolidColorBrush* {
        const std::uint64_t key = D2DColorBrushKey(color);
        auto it = brushCache_.find(key);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush)
                return nullptr;
            it = brushCache_.emplace(key, std::move(brush)).first;
        }
        return it->second.Get();
    };

    const QuickNavTheme& theme = lightTheme ? kQuickNavLight : kQuickNavDark;
    ID2D1SolidColorBrush* textBrush = getBrush(ToD2DColor(theme.itemText));
    if (!textBrush)
        return;

    const D2D1_POINT_2F origin = D2D1::Point2F(
        static_cast<float>(textRect.left),
        static_cast<float>(textRect.top));
    if (!lightTheme)
    {
        if (ID2D1SolidColorBrush* shadowBrush =
            getBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.38f)))
        {
            ctx->DrawTextLayout(
                D2D1::Point2F(origin.x + 1.0f, origin.y + 1.0f),
                layout.Get(), shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    ctx->DrawTextLayout(origin, layout.Get(), textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline void DesktopApp::DrawD2DText(ID2D1RenderTarget* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color)
{
    if (!ctx || !format || text.empty() || IsRectEmptyRect(rect)) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const std::uint64_t key = D2DColorBrushKey(color);
    auto it = brushCache_.find(key);
    if (it == brushCache_.end())
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush) return;
        it = brushCache_.emplace(key, std::move(brush)).first;
    }
    ctx->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        ToD2DRect(rect), it->second.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline void DesktopApp::DrawD2DTextEllipsis(ID2D1RenderTarget* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
    DWRITE_TEXT_ALIGNMENT hAlign, DWRITE_PARAGRAPH_ALIGNMENT vAlign, bool ellipsis)
{
    if (!ctx || !format || text.empty() || IsRectEmptyRect(rect) || !dwriteFactory_) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const std::uint64_t key = D2DColorBrushKey(color);
    auto it = brushCache_.find(key);
    if (it == brushCache_.end())
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush) return;
        it = brushCache_.emplace(key, std::move(brush)).first;
    }
    if (it == brushCache_.end() || !it->second) return;

    const float w = static_cast<float>(std::max<LONG>(1, rect.right - rect.left));
    const float h = static_cast<float>(std::max<LONG>(1, rect.bottom - rect.top));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(),
        static_cast<UINT32>(text.size()), format, w, h, &layout)) || !layout)
        return;
    layout->SetTextAlignment(hAlign);
    layout->SetParagraphAlignment(vAlign);
    if (ellipsis)
    {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format, &sign)) && sign)
            layout->SetTrimming(&trimming, sign.Get());
    }
    ctx->DrawTextLayout(
        D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)),
        layout.Get(), it->second.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline std::vector<std::wstring> DesktopApp::GetPopupItemKeys(const DesktopWidget& widget) const
{
    if (widget.type == DesktopWidgetType::Collection)
        return widget.itemKeys;
    return {};
}

inline RECT DesktopApp::GetCollectionPopupRect(const DesktopWidget& widget) const
{
    const GridPage* page = nullptr;
    for (const auto& p : gridPages_)
    {
        if (p.id == widget.gridCell.pageId)
        {
            page = &p;
            break;
        }
    }

    RECT work = page ? page->workArea : layoutWorkArea_;
    const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
    const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
    const int cellW = page ? page->cellWidth : kCellWidth;
    const int cellH = page ? page->cellHeight : kMinCellHeight;
    const int maxWidth = std::max(280, std::min(560, workWidth - 80));
    const int maxColumns = std::max(1, (maxWidth - kCollectionPopupPaddingX * 2) / std::max(1, cellW));
    const int itemCount = std::max(1, static_cast<int>(GetPopupItemKeys(widget).size()));
    int columns = std::clamp(std::min(itemCount, 5), 1, maxColumns);
    int rows = (itemCount + columns - 1) / columns;
    const int maxHeight = std::max(220, workHeight - 80);
    int width = kCollectionPopupPaddingX * 2 + columns * cellW;
    int height = kCollectionPopupHeaderHeight + rows * cellH + kCollectionPopupBottomPadding;
    if (height > maxHeight && columns < maxColumns)
    {
        columns = maxColumns;
        rows = (itemCount + columns - 1) / columns;
        width = kCollectionPopupPaddingX * 2 + columns * cellW;
        height = kCollectionPopupHeaderHeight + rows * cellH + kCollectionPopupBottomPadding;
    }
    height = std::min(height, maxHeight);

    int left = work.left + (workWidth - width) / 2;
    int top = work.top + (workHeight - height) / 2;
    if (popupHasAnchor_)
    {
        left = popupAnchorPoint_.x + 12;
        top = popupAnchorPoint_.y + 12;
        left = std::clamp(left, static_cast<int>(work.left + 12),
            static_cast<int>(std::max<LONG>(work.left + 12, work.right - width - 12)));
        top = std::clamp(top, static_cast<int>(work.top + 12),
            static_cast<int>(std::max<LONG>(work.top + 12, work.bottom - height - 12)));
    }
    return MakeRect(left, top, left + width, top + height);
}

inline RECT DesktopApp::GetCollectionPopupContentRect(const RECT& popup) const
{
    return MakeRect(
        popup.left + kCollectionPopupPaddingX,
        popup.top + kCollectionPopupHeaderHeight,
        popup.right - kCollectionPopupPaddingX,
        popup.bottom - kCollectionPopupBottomPadding);
}

inline int DesktopApp::GetCollectionPopupColumnCount(const RECT& popup) const
{
    RECT content = GetCollectionPopupContentRect(popup);
    int cellW = kCellWidth;
    for (const auto& page : gridPages_)
    {
        if (page.id == popupPageId_)
        {
            cellW = page.cellWidth;
            break;
        }
    }
    return std::max(1, static_cast<int>(content.right - content.left) / std::max(1, cellW));
}

inline int DesktopApp::GetCollectionPopupRowCount(const DesktopWidget& widget, const RECT& popup) const
{
    const int columns = GetCollectionPopupColumnCount(popup);
    const int itemCount = std::max(1, static_cast<int>(GetPopupItemKeys(widget).size()));
    return (itemCount + columns - 1) / columns;
}

inline int DesktopApp::GetCollectionPopupMaxScrollOffset(const DesktopWidget& widget, const RECT& popup) const
{
    RECT content = GetCollectionPopupContentRect(popup);
    int cellH = kMinCellHeight;
    for (const auto& page : gridPages_)
    {
        if (page.id == popupPageId_)
        {
            cellH = page.cellHeight;
            break;
        }
    }
    const int rows = GetCollectionPopupRowCount(widget, popup);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    return std::max(0, rows * std::max(1, cellH) - visibleHeight);
}

inline RECT DesktopApp::GetCollectionPopupItemRect(const RECT& popup, size_t linearIndex) const
{
    RECT content = GetCollectionPopupContentRect(popup);
    int cellW = kCellWidth;
    int cellH = kMinCellHeight;
    for (const auto& page : gridPages_)
    {
        if (page.id == popupPageId_)
        {
            cellW = page.cellWidth;
            cellH = page.cellHeight;
            break;
        }
    }
    const int columns = GetCollectionPopupColumnCount(popup);
    const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    return MakeRect(
        content.left + col * cellW,
        content.top + row * cellH - popupScrollOffset_,
        content.left + (col + 1) * cellW,
        content.top + (row + 1) * cellH - popupScrollOffset_);
}

inline bool DesktopApp::IsPointInsideOpenPopup(POINT point) const
{
    if (popupWidgetIndex_ >= widgets_.size()) return false;
    RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
    return PtInRect(&popup, point) != FALSE;
}

inline void DesktopApp::DrawCollectionPopup(ID2D1DeviceContext* ctx)
{
    if (!ctx || popupWidgetIndex_ >= widgets_.size()) return;

    const DesktopWidget& widget = widgets_[popupWidgetIndex_];
    popupPageId_ = widget.gridCell.pageId;
    std::vector<std::wstring> popupKeys = GetPopupItemKeys(widget);
    popupRect_ = GetCollectionPopupRect(widget);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widget, popupRect_));

    DrawD2DRoundedRectangle(ctx, popupRect_, 18.0f,
        D2D1::ColorF(0.08f, 0.10f, 0.13f, 1.0f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f), 1.4f);

    RECT titleRect = MakeRect(popupRect_.left + 22, popupRect_.top + 18,
        popupRect_.right - 22, popupRect_.top + 44);
    std::wstring title = widget.title.empty() ? L"集合" : widget.title;
    DrawD2DText(ctx, title, titleRect, itemTextFormat_.Get(),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));

    RECT content = GetCollectionPopupContentRect(popupRect_);
    ctx->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < popupKeys.size(); ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;

        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
        if (itemIndex == static_cast<size_t>(-1)) continue;

        bool hovered = !items_[itemIndex].selected && PtInRect(&itemRect, lastMousePoint_);
        DesktopIcon icon(&items_[itemIndex], nullptr, this);
        icon.Draw(ctx, itemRect, items_[itemIndex].selected ? 2 : (hovered ? 1 : 0));
    }
    ctx->PopAxisAlignedClip();

    // Scrollbar — same style as widget content areas
    int cellH = kMinCellHeight;
    for (const auto& page : gridPages_)
    {
        if (page.id == popupPageId_) { cellH = page.cellHeight; break; }
    }
    int columns = std::max(1, GetCollectionPopupColumnCount(popupRect_));
    int rows = (static_cast<int>(popupKeys.size()) + columns - 1) / columns;
    int contentHeight = rows * cellH;
    int visibleHeight = std::max(1, (int)(content.bottom - content.top));
    bool popupHovered = PtInRect(&popupRect_, lastMousePoint_);
    DrawScrollbarAt(ctx, content, contentHeight, visibleHeight, popupScrollOffset_, popupHovered);
}

extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);

// ── Static background layer (icons + widget chrome + popup) ──
inline void DesktopApp::DrawStaticBackground(ID2D1DeviceContext* ctx)
{
    // Desktop icons
    const bool mouseOverWidget = IsPointOverWidgetChrome(lastMousePoint_);
    for (auto& ooItem : items_oo_)
    {
        auto* icon = dynamic_cast<DesktopIcon*>(ooItem.get());
        if (!icon) continue;
        DesktopItem* di = icon->GetDesktopItem();
        if (!di || IsRectEmptyRect(di->bounds)) continue;
        if (dragSession_.IsActive() && !dragSession_.Items().empty() &&
            dragSession_.IsMoveAction() && di->selected)
            continue;

        const bool hovered = !mouseOverWidget && PtInRect(&di->bounds, lastMousePoint_) != FALSE;
        const bool selected = di->selected;
        int state = selected ? 2 : (hovered ? 1 : 0);
        icon->Draw(ctx, di->bounds, state);
    }

    // Widgets
    for (auto& widgetData : widgets_)
    {
        if (widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize)
        {
            if (mouseDownWidgetIndex_ < widgets_.size() &&
                &widgetData == &widgets_[mouseDownWidgetIndex_])
                continue;
        }

        if (widgetData.showOnHoverOnly && !dragSession_.IsActive() && !externalDragActive_
            && !(popupWidgetIndex_ < widgets_.size() && &widgetData == &widgets_[popupWidgetIndex_])
            && !PtInRect(&widgetData.bounds, lastMousePoint_))
            continue;

        bool drawn = false;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgetData) continue;
            wc->DrawChrome(ctx, lastMousePoint_);
            drawn = true;
            break;
        }
        if (drawn) continue;

        for (auto& ooItem : items_oo_)
        {
            auto* widget = dynamic_cast<Widget*>(ooItem.get());
            if (!widget || widget->GetWidgetData() != &widgetData) continue;
            widget->Draw(ctx, widgetData.bounds, widgetData.selected ? 2 : 0);
            break;
        }
    }

    DrawCollectionPopup(ctx);
}

// ── Dynamic overlays (drag preview, dragged items, marquee, nav) ──
inline void DesktopApp::DrawDynamicOverlays(ID2D1DeviceContext* ctx)
{
    // Widget drag/resize preview
    if ((widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize) && mouseDownWidgetIndex_ < widgets_.size())
    {
        GridCell cell = widgetPreviewCell_;
        GridSpan span = widgetPreviewSpan_;
        RECT previewBounds = GetGridRect(gridPages_, cell, span);

        bool widgetConflict = false;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (i == mouseDownWidgetIndex_) continue;
            const auto& ow = widgets_[i];
            if (ow.gridCell.pageId != cell.pageId) continue;
            if (cell.column + span.columns <= ow.gridCell.column) continue;
            if (ow.gridCell.column + ow.gridSpan.columns <= cell.column) continue;
            if (cell.row + span.rows <= ow.gridCell.row) continue;
            if (ow.gridCell.row + ow.gridSpan.rows <= cell.row) continue;
            widgetConflict = true; break;
        }

        bool ok = !widgetConflict && !cell.pageId.empty();
        float radius = 8.0f;
        D2D1::ColorF fill = ok ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.15f)
                              : D2D1::ColorF(1.0f, 0.30f, 0.30f, 0.18f);
        D2D1::ColorF border = ok ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.75f)
                                 : D2D1::ColorF(1.0f, 0.25f, 0.25f, 0.85f);

        DrawD2DRoundedRectangle(ctx, previewBounds, radius, fill, border, 2.0f);
    }

    // Drop preview (blue bars / green Handoff box)
    Container* targetContainer = dragSession_.TargetContainer();
    Slot* targetSlot = dragSession_.TargetSlot();
    HitRegion targetRegion = dragSession_.TargetRegion();
    if ((dragSession_.IsActive() || externalDragActive_) && targetContainer
        && targetRegion != HitRegion::None)
    {
        RECT clipViewport{};
        auto* wc = dynamic_cast<WidgetContainer*>(targetContainer);
        if (wc)
        {
            RECT bodyRect = wc->GetBodyRect();
            const bool popupTarget = popupWidgetIndex_ < widgets_.size() &&
                wc->GetWidgetData() == &widgets_[popupWidgetIndex_] &&
                targetSlot == popupDragTargetSlot_.get();
            if (popupTarget)
            {
                RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
                clipViewport = GetCollectionPopupContentRect(popup);
            }
            else
            {
                DesktopWidget* wd = wc->GetWidgetData();
                if (wd && wd->type == DesktopWidgetType::Collection && !wd->scrollContainerMode)
                {
                }
                else
                {
                    clipViewport = wc->GetContentViewportRect();
                    clipViewport.left = bodyRect.left;
                    clipViewport.right = bodyRect.right;
                }
            }
        }
        bool clipped = false;
        if (!IsRectEmptyRect(clipViewport))
        {
            ctx->PushAxisAlignedClip(ToD2DRect(clipViewport), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            clipped = true;
        }
        if (targetRegion == HitRegion::Handoff && targetSlot)
        {
            RECT bounds = targetSlot->GetBounds();
            DrawD2DRoundedRectangle(ctx, bounds, 6.0f,
                D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.15f),
                D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.60f), 2.0f);
        }
        else
        {
            targetContainer->DrawDropPreview(ctx, targetSlot, targetRegion);
        }
        if (clipped) ctx->PopAxisAlignedClip();
    }

    // Dragged items at offset
    if (dragSession_.IsActive() && !dragSession_.Items().empty())
    {
        POINT current = dragSession_.CurrentPoint();
        POINT mouseDown = dragSession_.MouseDownPoint();
        int dx = current.x - mouseDown.x;
        int dy = current.y - mouseDown.y;

        for (auto* item : dragSession_.Items())
        {
            if (!item) continue;
            RECT bounds = item->GetBounds();
            if (IsRectEmptyRect(bounds)) continue;

            RECT draggedBounds = MakeRect(
                bounds.left + dx, bounds.top + dy,
                bounds.right + dx, bounds.bottom + dy);
            item->Draw(ctx, draggedBounds, 3);
        }
    }

    if (marqueeActive_)
    {
        if (marqueeWidgetIndex_ < widgets_.size())
        {
            RECT viewport = GetMarqueeViewportRect();
            ctx->PushAxisAlignedClip(ToD2DRect(viewport),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            DrawD2DFilledRectangle(ctx, marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
            ctx->PopAxisAlignedClip();
        }
        else
        {
            DrawD2DFilledRectangle(ctx, marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
        }
    }

    DrawPageNavButtons(ctx);
    DrawPageNotify(ctx);
}

inline void DesktopApp::RenderFrame(ID2D1DeviceContext* ctx)
{
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const bool widgetPreviewActive =
        widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize;
    if (dragSession_.IsActive() || widgetPreviewActive)
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        UINT w = std::max<LONG>(1, client.right - client.left);
        UINT h = std::max<LONG>(1, client.bottom - client.top);

        bool cacheReady = dragRenderCache_.Ensure(d2dDevice_.Get(), D2D1_SIZE_U{ w, h },
            dragSession_.StaticSceneRevision(),
            [&](ID2D1DeviceContext* cacheCtx) { DrawStaticBackground(cacheCtx); });
        brushCache_.clear();
        brushCacheContext_ = ctx;
        if (cacheReady)
            dragRenderCache_.Draw(ctx);
        else
            DrawStaticBackground(ctx);
        DrawDynamicOverlays(ctx);
        return;
    }

    // ── Normal path (not dragging) ────────────────────────────
    DrawStaticBackground(ctx);
    DrawDynamicOverlays(ctx);
}

inline void DesktopApp::GetNavButtonRects(RECT& outPrev, RECT& outNext) const
{
    outPrev = {};
    outNext = {};
    if (gridPages_.empty()) return;

    const GridPage* targetPage = nullptr;
    for (const auto& page : gridPages_)
    {
        if (!lastMonitorPageId_.empty() && page.id == lastMonitorPageId_)
        {
            targetPage = &page;
            break;
        }
    }
    if (!targetPage)
    {
        // 回退到渲染顺序的末屏（双锚点下可能不是 bounds.left 最右者）
        std::vector<size_t> order = BuildMonitorRenderOrder();
        if (!order.empty()) targetPage = &gridPages_[order.back()];
    }
    if (!targetPage) targetPage = &gridPages_.back();

    constexpr LONG buttonW = 40, buttonH = 96, padX = 0;
    const LONG cy = (targetPage->workArea.top + targetPage->workArea.bottom) / 2;
    const LONG halfH = buttonH / 2;

    outPrev = MakeRect(
        targetPage->workArea.left + padX, cy - halfH,
        targetPage->workArea.left + padX + buttonW, cy + halfH);
    outNext = MakeRect(
        targetPage->workArea.right - padX - buttonW, cy - halfH,
        targetPage->workArea.right - padX, cy + halfH);
}

inline void DesktopApp::DrawPageNavButtons(ID2D1DeviceContext* ctx)
{
    if (MaxPageOffset() <= 0) return;
    if (gridPages_.empty()) return;

    // 默认隐藏翻页按钮；仅在换页通知期间或拖拽悬停时显示，提示用户按钮位置
    const bool dragging = (dragSession_.IsActive() &&
        (!dragSession_.Items().empty() || externalDragActive_ || selfDragActive_)) ||
        widgetAction_ == WidgetAction::Move;
    const bool hovering = (navHoverSide_ != 0);
    if (!pageNotifyActive_ && !dragging && !hovering) return;

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    auto drawArrow = [&](const RECT& rect, const std::wstring& arrow, bool enabled, bool hovered) {
        D2D1_RECT_F d2dRect = D2D1::RectF(
            static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right), static_cast<float>(rect.bottom));

        if (enabled)
        {
            float bgAlpha = hovered ? 1.0f : 0.85f;
            ComPtr<ID2D1SolidColorBrush> bg;
            ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, bgAlpha), &bg);
            if (bg) ctx->FillRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), bg.Get());

            ComPtr<ID2D1SolidColorBrush> borderBrush;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f), &borderBrush);
            if (borderBrush) ctx->DrawRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), borderBrush.Get(), 1.0f);
        }
        else
        {
            // 置灰：半透明深色背景
            ComPtr<ID2D1SolidColorBrush> bg;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.35f), &bg);
            if (bg) ctx->FillRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), bg.Get());
        }

        float textAlpha = enabled ? (hovered ? 1.0f : 0.65f) : 0.3f;
        ComPtr<ID2D1SolidColorBrush> textBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, textAlpha), &textBrush);
        if (!textBrush || !dwriteFactory_) return;

        float w = static_cast<float>(rect.right - rect.left);
        float h = static_cast<float>(rect.bottom - rect.top);

        ComPtr<IDWriteTextFormat> arrowFmt;
        dwriteFactory_->CreateTextFormat(L"Segoe UI Symbol", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"", &arrowFmt);
        if (!arrowFmt) return;
        arrowFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        arrowFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(arrow.c_str(),
            static_cast<UINT32>(arrow.size()), arrowFmt.Get(), w, h, &layout)) && layout)
        {
            ctx->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)),
                layout.Get(), textBrush.Get());
        }
    };

    bool hoverPrev = (navHoverSide_ == -1);
    bool hoverNext = (navHoverSide_ == 1);

    drawArrow(prevRect, L"\u25C0", hasPrev, dragging || hoverPrev);
    drawArrow(nextRect, L"\u25B6", hasNext, dragging || hoverNext);
}

inline void DesktopApp::DrawShortcutArrowOverlay(ID2D1RenderTarget* ctx, RECT iconRect, float alpha)
{
    if (!ctx) return;

    auto createArrowBitmap = [&](ComPtr<ID2D1Bitmap>& outBitmap, SIZE& outSize) -> bool {
        if (outBitmap)
            return true;

        SHSTOCKICONINFO sii{};
        sii.cbSize = sizeof(sii);
        if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) || !sii.hIcon)
            return false;

        int w = GetSystemMetrics(SM_CXICON);
        int h = GetSystemMetrics(SM_CYICON);
        if (w <= 0) w = 32;
        if (h <= 0) h = 32;

        SIZE bitmapSize{};
        HBITMAP dib = CreateAlphaBitmapFromIcon(sii.hIcon, w, h, bitmapSize);
        if (!dib)
        {
            DestroyIcon(sii.hIcon);
            return false;
        }

        DIBSECTION ds{};
        GetObjectW(dib, sizeof(ds), &ds);

        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        ComPtr<ID2D1Bitmap> bitmap;
        HRESULT hr = ctx->CreateBitmap(D2D1::SizeU(w, h), ds.dsBm.bmBits,
            static_cast<UINT32>(ds.dsBm.bmWidthBytes), props, &bitmap);

        DeleteObject(dib);
        DestroyIcon(sii.hIcon);

        if (FAILED(hr) || !bitmap)
            return false;

        outBitmap = std::move(bitmap);
        outSize = bitmapSize;
        return true;
    };

    ID2D1Bitmap* arrowBitmap = nullptr;

    ComPtr<ID2D1DeviceContext> deviceContext;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&deviceContext))) && deviceContext)
    {
        if (!createArrowBitmap(shortcutArrowBitmap_, shortcutArrowBitmapSize_))
            return;
        arrowBitmap = shortcutArrowBitmap_.Get();
    }
    else
    {
        // 非 device-context 渲染目标已不再使用；快捷导航走 DComp 后 ctx 必为 device context。
        return;
    }

    if (!arrowBitmap) return;

    float scale = static_cast<float>(iconRect.bottom - iconRect.top) / 64.0f;
    int arrowSz = static_cast<int>(30.0f * scale + 0.5f);
    if (arrowSz < 10)
        arrowSz = 10;
    int arrowX = iconRect.left;
    int arrowY = iconRect.bottom - arrowSz;

    D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(arrowX),
        static_cast<float>(arrowY),
        static_cast<float>(arrowX + arrowSz),
        static_cast<float>(arrowY + arrowSz));

    ctx->DrawBitmap(arrowBitmap, dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

inline void DesktopApp::DrawPlaceholderIcon(ID2D1RenderTarget* ctx, int sysIconIndex, RECT iconRect, float alpha)
{
    if (!ctx || sysIconIndex < 0) return;

    // 快捷导航改走 DComp 后，ctx 必为 ID2D1DeviceContext（与桌面同源 d2dDevice_）。
    // 非 device-context 路径已废弃，直接返回以避免在错误设备上创建位图。
    ComPtr<ID2D1DeviceContext> deviceContext;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&deviceContext))) || !deviceContext || !d2dContext_)
        return;
    ID2D1RenderTarget* creationTarget = static_cast<ID2D1RenderTarget*>(d2dContext_.Get());
    auto& cache = placeholderIconCache_;

    auto cached = cache.find(sysIconIndex);
    if (cached == cache.end())
    {
        ComPtr<IImageList> imageList;
        HRESULT hr = SHGetImageList(SHIL_JUMBO, IID_IImageList,
            reinterpret_cast<void**>(imageList.GetAddressOf()));
        if (FAILED(hr) || !imageList)
        {
            imageList.Reset();
            hr = SHGetImageList(SHIL_EXTRALARGE, IID_IImageList,
                reinterpret_cast<void**>(imageList.GetAddressOf()));
        }
        if (FAILED(hr) || !imageList)
        {
            imageList.Reset();
            hr = SHGetImageList(SHIL_LARGE, IID_IImageList,
                reinterpret_cast<void**>(imageList.GetAddressOf()));
        }
        if (FAILED(hr) || !imageList)
            return;

        HICON icon = nullptr;
        if (FAILED(imageList->GetIcon(sysIconIndex,
                ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon)) || !icon)
            return;

        SIZE bitmapSize{};
        HBITMAP alphaBitmap = CreateAlphaBitmapFromIcon(
            icon, kIconBitmapSize, kIconBitmapSize, bitmapSize);
        DestroyIcon(icon);
        if (!alphaBitmap)
            return;

        DIBSECTION ds{};
        if (GetObjectW(alphaBitmap, sizeof(ds), &ds) == 0 ||
            !ds.dsBm.bmBits || ds.dsBm.bmWidth <= 0 || ds.dsBm.bmHeight == 0)
        {
            DeleteObject(alphaBitmap);
            return;
        }

        const UINT width = static_cast<UINT>(ds.dsBm.bmWidth);
        const UINT height = static_cast<UINT>(std::abs(ds.dsBm.bmHeight));
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap> bitmap;
        HRESULT createHr = creationTarget->CreateBitmap(D2D1::SizeU(width, height),
            ds.dsBm.bmBits, static_cast<UINT32>(ds.dsBm.bmWidthBytes), props, &bitmap);
        DeleteObject(alphaBitmap);
        if (FAILED(createHr) || !bitmap)
            return;

        cached = cache.emplace(sysIconIndex, std::move(bitmap)).first;
    }

    D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
        static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
    ctx->DrawBitmap(cached->second.Get(), dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

inline void DesktopApp::DrawQuickNavSysIcon(ID2D1RenderTarget* ctx, int sysIconIndex, RECT dstRect)
{
    if (!ctx || sysIconIndex < 0) return;
    // 仅 ID2D1DeviceContext（与桌面同源 d2dDevice_）才支持 CreateBitmap/共享。
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&dc))) || !dc) return;

    auto cached = quickNavSysIconCache_.find(sysIconIndex);
    if (cached == quickNavSysIconCache_.end())
    {
        // 用 EXTRALARGE(48px) 源：内容填满画布，避免 JUMBO 部分图标的透明留白导致缩放后偏小/偏角。
        ComPtr<IImageList> imageList;
        HRESULT hr = SHGetImageList(SHIL_EXTRALARGE, IID_IImageList,
            reinterpret_cast<void**>(imageList.GetAddressOf()));
        if (FAILED(hr) || !imageList)
        {
            imageList.Reset();
            hr = SHGetImageList(SHIL_LARGE, IID_IImageList,
                reinterpret_cast<void**>(imageList.GetAddressOf()));
        }
        if (FAILED(hr) || !imageList) return;

        HICON icon = nullptr;
        if (FAILED(imageList->GetIcon(sysIconIndex,
                ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon)) || !icon)
            return;

        const int srcSize = 48;
        SIZE bitmapSize{};
        HBITMAP alphaBitmap = CreateAlphaBitmapFromIcon(icon, srcSize, srcSize, bitmapSize);
        DestroyIcon(icon);
        if (!alphaBitmap) return;

        DIBSECTION ds{};
        if (GetObjectW(alphaBitmap, sizeof(ds), &ds) == 0 ||
            !ds.dsBm.bmBits || ds.dsBm.bmWidth <= 0 || ds.dsBm.bmHeight == 0)
        {
            DeleteObject(alphaBitmap);
            return;
        }

        const UINT w = static_cast<UINT>(ds.dsBm.bmWidth);
        const UINT h = static_cast<UINT>(std::abs(ds.dsBm.bmHeight));
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap> bitmap;
        HRESULT createHr = ctx->CreateBitmap(D2D1::SizeU(w, h),
            ds.dsBm.bmBits, static_cast<UINT32>(ds.dsBm.bmWidthBytes), props, &bitmap);
        DeleteObject(alphaBitmap);
        if (FAILED(createHr) || !bitmap) return;

        cached = quickNavSysIconCache_.emplace(sysIconIndex, std::move(bitmap)).first;
    }

    D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(dstRect.left), static_cast<float>(dstRect.top),
        static_cast<float>(dstRect.right), static_cast<float>(dstRect.bottom));
    ctx->DrawBitmap(cached->second.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

/**
 * @brief 触发换页通知（记录文本与时间戳，启动重绘定时器）。
 * @param text 通知文本（如"第3页"）。
 */
inline void DesktopApp::ShowPageNotify(const std::wstring& text)
{
    if (text.empty()) return;
    pageNotifyText_ = text;
    pageNotifyStartTick_ = GetTickCount();
    pageNotifyActive_ = true;
    SetTimer(hwnd_, kPageNotifyTimerId, kPageNotifyTimerIntervalMs, nullptr);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 绘制换页通知覆盖层（左上角角标，类似电视台换台）。
 *
 * 显示 kPageNotifyVisibleMs 毫秒，最后 kPageNotifyFadeMs 毫秒淡出。
 * 位置：末屏左上角（若有末屏），否则主屏左上角。
 * @param ctx D2D 设备上下文。
 */
inline void DesktopApp::DrawPageNotify(ID2D1DeviceContext* ctx)
{
    if (!ctx || !pageNotifyActive_ || pageNotifyText_.empty()) return;

    const DWORD now = GetTickCount();
    const DWORD elapsed = now - pageNotifyStartTick_;
    if (elapsed >= kPageNotifyVisibleMs)
    {
        pageNotifyActive_ = false;
        pageNotifyText_.clear();
        KillTimer(hwnd_, kPageNotifyTimerId);
        return;
    }

    // 计算透明度：前 kPageNotifyFadeMs 淡入，最后 kPageNotifyFadeMs 淡出
    float alpha = 1.0f;
    const DWORD fadeMs = kPageNotifyFadeMs;
    if (elapsed < fadeMs)
        alpha = static_cast<float>(elapsed) / static_cast<float>(fadeMs);
    else if (elapsed > kPageNotifyVisibleMs - fadeMs)
        alpha = static_cast<float>(kPageNotifyVisibleMs - elapsed) / static_cast<float>(fadeMs);
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    // 定位：渲染顺序的末屏显示器（不依赖 lastMonitorPageId_，单屏时也能定位）
    const GridPage* targetPage = nullptr;
    {
        std::vector<size_t> order = BuildMonitorRenderOrder();
        if (!order.empty()) targetPage = &gridPages_[order.back()];
    }
    if (!targetPage) return;

    auto* dwrite = GetDWriteFactory();
    if (!dwrite) return;

    // 大号字体
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 42.0f, L"", &fmt)) || !fmt)
        return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 先用大尺寸测量文本
    ComPtr<IDWriteTextLayout> measureLayout;
    if (FAILED(dwrite->CreateTextLayout(pageNotifyText_.c_str(),
        static_cast<UINT32>(pageNotifyText_.size()),
        fmt.Get(), 2000.0f, 200.0f, &measureLayout)) || !measureLayout)
        return;

    DWRITE_TEXT_METRICS metrics{};
    measureLayout->GetMetrics(&metrics);

    // 背景圆角矩形（半透明深色）
    const float padX = 28.0f;
    const float padY = 14.0f;
    const float boxW = metrics.width + padX * 2.0f;
    const float boxH = metrics.height + padY * 2.0f;
    const float boxLeft = static_cast<float>(targetPage->workArea.left) + 24.0f;
    const float boxTop = static_cast<float>(targetPage->workArea.top) + 24.0f;

    // 用实际文本尺寸重建布局，使居中对齐生效
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(pageNotifyText_.c_str(),
        static_cast<UINT32>(pageNotifyText_.size()),
        fmt.Get(), metrics.width, metrics.height, &layout)) || !layout)
        return;

    ComPtr<ID2D1SolidColorBrush> bgBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.05f, 0.05f, 0.08f, 0.72f * alpha), &bgBrush);
    if (bgBrush)
    {
        D2D1_RECT_F bgRect = D2D1::RectF(boxLeft, boxTop, boxLeft + boxW, boxTop + boxH);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(bgRect, 10.0f, 10.0f), bgBrush.Get());
    }

    // 边框（细高亮线）
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.4f, 0.6f, 1.0f, 0.5f * alpha), &borderBrush);
    if (borderBrush)
    {
        D2D1_RECT_F bgRect = D2D1::RectF(boxLeft, boxTop, boxLeft + boxW, boxTop + boxH);
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(bgRect, 10.0f, 10.0f),
            borderBrush.Get(), 1.5f);
    }

    // 文本（带阴影）
    const float textX = boxLeft + padX;
    const float textY = boxTop + padY;

    ComPtr<ID2D1SolidColorBrush> shadowBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f * alpha), &shadowBrush);
    if (shadowBrush)
        ctx->DrawTextLayout(D2D1::Point2F(textX + 2.0f, textY + 2.0f),
            layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    ComPtr<ID2D1SolidColorBrush> textBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.97f * alpha), &textBrush);
    if (textBrush)
        ctx->DrawTextLayout(D2D1::Point2F(textX, textY),
            layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline void DesktopApp::DrawHiddenHintOverlay(ID2D1DeviceContext* ctx)
{
    if (!ctx || !showHiddenHint_) return;

    auto* dwrite = GetDWriteFactory();
    if (!dwrite) return;

    RECT workArea{};
    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        const GridPage* page = GridPageFromScreenPoint(cursor);
        if (page) workArea = page->workArea;
    }
    if (IsRectEmptyRect(workArea))
    {
        if (const GridPage* firstPage = GetFirstPageGridPage())
            workArea = firstPage->workArea;
        if (IsRectEmptyRect(workArea))
        {
            workArea.left = 0;
            workArea.top = 0;
            workArea.right = GetSystemMetrics(SM_CXSCREEN);
            workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    const std::wstring hintText = L"双击取消隐藏桌面，可在设置中关闭此功能";

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &fmt)) || !fmt)
        return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Measure text width using a temporary layout
    ComPtr<IDWriteTextLayout> measureLayout;
    if (SUCCEEDED(dwrite->CreateTextLayout(hintText.c_str(),
        static_cast<UINT32>(hintText.size()), fmt.Get(), 2000.0f, 40.0f, &measureLayout)) && measureLayout)
    {
        DWRITE_TEXT_METRICS metrics{};
        measureLayout->GetMetrics(&metrics);

        constexpr float hintPadding = 24.0f;
        constexpr float hintHeight = 36.0f;
        constexpr float marginTop = 60.0f;

        const float textW = metrics.width + hintPadding * 2.0f;
        const int areaW = workArea.right - workArea.left;

        RECT hintRect = MakeRect(
            static_cast<int>(workArea.left + (areaW - textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop),
            static_cast<int>(workArea.left + (areaW + textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop + hintHeight));

        DrawD2DRoundedRectangle(ctx, hintRect, 10.0f,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f),
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);

        DrawD2DText(ctx, hintText, hintRect, fmt.Get(),
            D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.90f));
    }
}

inline void DesktopApp::DrawWidgetAddedHintOverlay(ID2D1DeviceContext* ctx)
{
    if (!ctx || !showWidgetAddedHint_) return;

    auto* dwrite = GetDWriteFactory();
    if (!dwrite) return;

    RECT workArea{};
    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        const GridPage* page = GridPageFromScreenPoint(cursor);
        if (page) workArea = page->workArea;
    }
    if (IsRectEmptyRect(workArea))
    {
        if (const GridPage* firstPage = GetFirstPageGridPage())
            workArea = firstPage->workArea;
        if (IsRectEmptyRect(workArea))
        {
            workArea.left = 0;
            workArea.top = 0;
            workArea.right = GetSystemMetrics(SM_CXSCREEN);
            workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    const std::wstring hintText = L"拖动组件底部可移动组件位置，拖动组件右下角圆点可调整组件大小";

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &fmt)) || !fmt)
        return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteTextLayout> measureLayout;
    if (SUCCEEDED(dwrite->CreateTextLayout(hintText.c_str(),
        static_cast<UINT32>(hintText.size()), fmt.Get(), 2000.0f, 40.0f, &measureLayout)) && measureLayout)
    {
        DWRITE_TEXT_METRICS metrics{};
        measureLayout->GetMetrics(&metrics);

        constexpr float hintPadding = 24.0f;
        constexpr float hintHeight = 36.0f;
        constexpr float marginTop = 60.0f;

        const float textW = metrics.width + hintPadding * 2.0f;
        const int areaW = workArea.right - workArea.left;

        RECT hintRect = MakeRect(
            static_cast<int>(workArea.left + (areaW - textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop),
            static_cast<int>(workArea.left + (areaW + textW) / 2.0f),
            static_cast<int>(workArea.top + marginTop + hintHeight));

        DrawD2DRoundedRectangle(ctx, hintRect, 10.0f,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f),
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);

        DrawD2DText(ctx, hintText, hintRect, fmt.Get(),
            D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.90f));
    }
}
