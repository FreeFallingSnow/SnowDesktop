#pragma once
// Inline implementations for DesktopApp — Graphics & Rendering.
// This file is included by app_oo.h after the class definition.

#include <cstring>

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
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice_, &fl, nullptr);
    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3dDevice_, &fl, nullptr);
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

        const int menuHeight = -std::max(9, GetSystemMetrics(SM_CYMENUCHECK) * 7 / 10);
        faMenuFont_ = CreateFontW(menuHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Font Awesome 6 Free Solid");
    }

    return true;
}

inline void DesktopApp::RecreateItemTextFormat()
{
    if (!dwriteFactory_) return;
    itemTextLayoutCache_.clear();
    float fontSize = itemFontSize_;
    float lineHeight = fontSize * 7.0f / 6.0f;
    float baseline = fontSize * 5.0f / 6.0f;
    dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
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
        if (FAILED(hr)) return hr;
        dcompDevice_->Commit();

        dcompSurface_ = surface;
        compositionWidth_ = width;
        compositionHeight_ = height;
        return S_OK;
    }

inline void DesktopApp::OnPaint()
    {
        if (FAILED(CreateOrResizeCompositionSurface())) return;

        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        HRESULT hr = dcompSurface_->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&rawContext), &updateOffset);
        if (FAILED(hr)) return;

        ComPtr<ID2D1DeviceContext> context;
        context.Attach(rawContext);
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x), static_cast<float>(updateOffset.y)));
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        RenderFrame(context.Get());

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();

        hr = dcompSurface_->EndDraw();
        if (SUCCEEDED(hr)) dcompDevice_->Commit();
    }

inline D2D1_RECT_F DesktopApp::ToD2DRect(const RECT& r)
{
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
        static_cast<float>(r.right), static_cast<float>(r.bottom));
}

inline std::uint64_t D2DColorBrushKey(const D2D1_COLOR_F& c)
{
    std::uint32_t r = 0, g = 0, b = 0, a = 0;
    std::memcpy(&r, &c.r, sizeof(r));
    std::memcpy(&g, &c.g, sizeof(g));
    std::memcpy(&b, &c.b, sizeof(b));
    std::memcpy(&a, &c.a, sizeof(a));
    return static_cast<std::uint64_t>(r) ^
        (static_cast<std::uint64_t>(g) << 16) ^
        (static_cast<std::uint64_t>(b) << 32) ^
        (static_cast<std::uint64_t>(a) << 48);
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

inline void DesktopApp::DrawD2DRoundedRectangle(ID2D1DeviceContext* ctx, RECT rect, float radius,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth)
{
    if (!ctx || IsRectEmptyRect(rect)) return;

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
        std::uint64_t k = D2DColorBrushKey(stroke) ^ 0x8000000080000000ULL;
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

inline void DesktopApp::DrawD2DFilledRectangle(ID2D1DeviceContext* ctx, RECT rect,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke)
{
    if (!ctx || IsRectEmptyRect(rect)) return;

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
        std::uint64_t k = D2DColorBrushKey(stroke) ^ 0x8000000080000000ULL;
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

inline void DesktopApp::DrawItemText(ID2D1DeviceContext* context, RECT bounds,
    const std::wstring& text, bool selected, float opacity)
{
    if (!dwriteFactory_ || !itemTextFormat_ || text.empty()) return;

    RECT textRect = GetItemTextRect(bounds, selected);
    float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
    float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));

    const float layoutScale = GetItemLayoutScale(bounds);
    const int scaleKey = static_cast<int>(std::round(layoutScale * 1000.0f));
    std::wstring layoutKey = text + L"\x1f" +
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
            itemFontSize_ * 7.0f / 6.0f * layoutScale, itemFontSize_ * 5.0f / 6.0f * layoutScale);
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        if (metrics.lineCount == 1)
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layoutIt = itemTextLayoutCache_.emplace(std::move(layoutKey), std::move(layout)).first;
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
    ID2D1SolidColorBrush* shadowBrush =
        getBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f * opacity));
    ID2D1SolidColorBrush* textBrush =
        getBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity));

    const float tx = static_cast<float>(textRect.left);
    float ty = static_cast<float>(textRect.top);
    DWRITE_TEXT_METRICS metrics{};
    layoutIt->second->GetMetrics(&metrics);
    if (metrics.lineCount == 1 && selected)
    {
        RECT cr = GetItemTextRect(bounds, false);
        float collapsedH = static_cast<float>(cr.bottom - cr.top);
        ty = cr.top + (collapsedH - th) * 0.5f;
    }
    if (shadowBrush)
    {
        const float shadowOffset = std::max(0.5f, layoutScale);
        const D2D1_POINT_2F offsets[] = {
            { tx - shadowOffset, ty }, { tx + shadowOffset, ty },
            { tx, ty - shadowOffset }, { tx, ty + shadowOffset },
        };
        for (auto& pt : offsets)
            context->DrawTextLayout(pt, layoutIt->second.Get(), shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (textBrush)
        context->DrawTextLayout(D2D1::Point2F(tx, ty), layoutIt->second.Get(), textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline void DesktopApp::DrawD2DText(ID2D1DeviceContext* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color)
{
    if (!ctx || !format || text.empty() || IsRectEmptyRect(rect)) return;
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

inline std::vector<size_t> DesktopApp::GetQuickNavigationCollectionIndices() const
{
    std::vector<size_t> result;
    std::unordered_set<size_t> seen;

    for (const auto& id : navTabOrder_)
    {
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (widgets_[i].type == DesktopWidgetType::Collection &&
                widgets_[i].id == id && !seen.count(i))
            {
                result.push_back(i);
                seen.insert(i);
                break;
            }
        }
    }
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].type == DesktopWidgetType::Collection && !seen.count(i))
        {
            result.push_back(i);
            seen.insert(i);
        }
    }
    return result;
}

inline std::vector<std::wstring> DesktopApp::GetQuickNavigationItemKeys() const
{
    std::vector<std::wstring> result;
    std::unordered_set<std::wstring> seen;
    auto appendKey = [&](const std::wstring& key) {
        if (FindItemIndexByKey(key) == static_cast<size_t>(-1))
            return;
        std::wstring normalized = ToUpperInvariant(key);
        if (normalized.empty() || seen.contains(normalized))
            return;
        seen.insert(std::move(normalized));
        result.push_back(key);
    };

    if (quickNavigationActiveWidgetIndex_ < widgets_.size() &&
        widgets_[quickNavigationActiveWidgetIndex_].type == DesktopWidgetType::Collection)
    {
        for (const auto& key : widgets_[quickNavigationActiveWidgetIndex_].itemKeys)
            appendKey(key);
        return result;
    }

    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    for (size_t ci : collectionIndices)
    {
        for (const auto& key : widgets_[ci].itemKeys)
            appendKey(key);
    }
    return result;
}

inline std::vector<DesktopApp::QuickNavigationEntry> DesktopApp::GetQuickNavigationEntries() const
{
    std::vector<QuickNavigationEntry> result;
    std::unordered_set<std::wstring> seenDesktop;
    std::wstring query = ToUpperInvariant(quickNavigationSearchText_);

    auto matches = [&](const std::wstring& name, const std::wstring& path, const std::wstring& source) {
        if (query.empty()) return true;
        return ToUpperInvariant(name).find(query) != std::wstring::npos ||
            ToUpperInvariant(path).find(query) != std::wstring::npos ||
            ToUpperInvariant(source).find(query) != std::wstring::npos;
    };
    auto appendDesktop = [&](size_t itemIndex, const std::wstring& source) {
        if (itemIndex >= items_.size()) return;
        const DesktopItem& item = items_[itemIndex];
        std::wstring key = ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        if (key.empty() || seenDesktop.contains(key)) return;
        if (!matches(item.name, item.parsingName, source)) return;
        seenDesktop.insert(std::move(key));

        QuickNavigationEntry entry;
        entry.kind = QuickNavigationEntry::Kind::DesktopItem;
        entry.itemIndex = itemIndex;
        entry.name = item.name;
        entry.path = item.parsingName;
        entry.source = source;
        entry.iconBitmap = item.iconBitmap;
        result.push_back(std::move(entry));
    };

    if (query.empty())
    {
        for (const auto& key : GetQuickNavigationItemKeys())
            appendDesktop(FindItemIndexByKey(key), L"集合");

        bool isAllTab = quickNavigationActiveWidgetIndex_ >= widgets_.size() ||
            widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::Collection;
        if (isAllTab)
        {
            std::unordered_set<std::wstring> collectionKeys;
            for (const auto& widget : widgets_)
            {
                if (widget.type != DesktopWidgetType::Collection) continue;
                for (const auto& key : widget.itemKeys)
                    collectionKeys.insert(ToUpperInvariant(key));
            }

            auto isLnkOrUrl = [](const std::wstring& path) -> bool {
                if (path.size() < 4) return false;
                std::wstring ext = path.substr(path.size() - 4);
                for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
                return ext == L".LNK" || ext == L".URL";
            };

            for (size_t i = 0; i < items_.size(); ++i)
            {
                const DesktopItem& item = items_[i];
                std::wstring key = ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
                if (collectionKeys.contains(key)) continue;
                if (!isLnkOrUrl(item.parsingName)) continue;
                appendDesktop(i, L"自由桌面");
            }
        }

        return result;
    }

    for (size_t i = 0; i < items_.size(); ++i)
        appendDesktop(i, L"桌面");

    for (size_t ci : GetQuickNavigationCollectionIndices())
    {
        const DesktopWidget& widget = widgets_[ci];
        std::wstring source = widget.title.empty() ? L"集合" : widget.title;
        for (const auto& key : widget.itemKeys)
            appendDesktop(FindItemIndexByKey(key), source);
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const DesktopWidget& widget = widgets_[wi];
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        std::wstring source = widget.title.empty() ? L"文件夹映射" : widget.title;
        for (size_t ei = 0; ei < widget.folderEntries.size(); ++ei)
        {
            const FolderEntry& entryData = widget.folderEntries[ei];
            if (!matches(entryData.name, entryData.fullPath, source))
                continue;

            QuickNavigationEntry entry;
            entry.kind = QuickNavigationEntry::Kind::FolderEntry;
            entry.widgetIndex = wi;
            entry.folderEntryIndex = ei;
            entry.name = entryData.name;
            entry.path = entryData.fullPath;
            entry.source = source;
            entry.iconBitmap = entryData.iconBitmap;
            result.push_back(std::move(entry));
        }
    }

    return result;
}

inline RECT DesktopApp::GetQuickNavigationRect() const
{
    RECT work{};
    bool foundWorkArea = false;
    POINT anchor = quickNavigationOpenPoint_;
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, anchor) || PtInRect(&page.workArea, anchor))
        {
            work = page.workArea;
            foundWorkArea = true;
            break;
        }
    }

    if (!foundWorkArea)
    {
        POINT screenAnchor{ anchor.x + virtualLeft_, anchor.y + virtualTop_ };
        HMONITOR monitor = MonitorFromPoint(screenAnchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
        {
            work = MakeRect(
                monitorInfo.rcWork.left - virtualLeft_,
                monitorInfo.rcWork.top - virtualTop_,
                monitorInfo.rcWork.right - virtualLeft_,
                monitorInfo.rcWork.bottom - virtualTop_);
            foundWorkArea = true;
        }
    }

    if (!foundWorkArea)
        work = layoutWorkArea_;
    if (IsRectEmptyRect(work))
        work = MakeRect(0, 0, virtualWidth_, virtualHeight_);

    const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
    const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
    const int widthLimit = std::max(QuickNavScale(320), workWidth - QuickNavScale(48));
    const int heightLimit = std::max(QuickNavScale(280), workHeight - QuickNavScale(48));
    const int width = std::min(widthLimit, std::max(QuickNavScale(520), std::min(QuickNavScale(860), workWidth - QuickNavScale(120))));
    const int height = std::min(heightLimit, std::max(QuickNavScale(360), std::min(QuickNavScale(620), workHeight - QuickNavScale(120))));
    const int left = work.left + (workWidth - width) / 2;
    const int top = work.top + (workHeight - height) / 2;
    return MakeRect(left, top, left + width, top + height);
}

inline RECT DesktopApp::GetQuickNavigationSearchRect(const RECT& overlay) const
{
    return MakeRect(overlay.left + QuickNavScale(16), overlay.top + QuickNavScale(54),
        overlay.right - QuickNavScale(16), overlay.top + QuickNavScale(86));
}

inline RECT DesktopApp::GetQuickNavigationTabsRect(const RECT& overlay) const
{
    return MakeRect(overlay.left + QuickNavScale(22), overlay.top + QuickNavScale(100),
        overlay.right - QuickNavScale(22), overlay.top + QuickNavScale(134));
}

inline RECT DesktopApp::GetQuickNavigationContentRect(const RECT& overlay) const
{
    if (!quickNavigationSearchText_.empty())
        return MakeRect(overlay.left + QuickNavScale(12), overlay.top + QuickNavScale(102),
            overlay.right - QuickNavScale(12), overlay.bottom - QuickNavScale(12));
    return MakeRect(overlay.left + QuickNavScale(12), overlay.top + QuickNavScale(148),
        overlay.right - QuickNavScale(12), overlay.bottom - QuickNavScale(12));
}

inline int DesktopApp::GetQuickNavigationTabStripContentWidth(const RECT& overlay) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    const size_t tabCount = collectionIndices.size() + 1;
    if (tabCount == 0) return 0;

    const int gap = QuickNavScale(8);
    const int available = std::max(1, static_cast<int>(tabs.right - tabs.left));
    int tabWidth = std::clamp((available - gap * static_cast<int>(tabCount - 1)) / static_cast<int>(tabCount),
        QuickNavScale(72), QuickNavScale(150));
    return static_cast<int>(tabCount) * tabWidth + static_cast<int>(tabCount - 1) * gap;
}

inline int DesktopApp::GetQuickNavigationMaxTabScrollOffset(const RECT& overlay) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    const int available = std::max(1, static_cast<int>(tabs.right - tabs.left));
    return std::max(0, GetQuickNavigationTabStripContentWidth(overlay) - available);
}

inline int DesktopApp::GetQuickNavigationTabWidth() const
{
    RECT overlay = quickNavigationRect_;
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    const size_t tabCount = collectionIndices.size() + 1;
    if (tabCount == 0) return QuickNavScale(92);
    const int gap = QuickNavScale(8);
    const int available = std::max(1, static_cast<int>(tabs.right - tabs.left));
    return std::clamp((available - gap * static_cast<int>(tabCount - 1)) / static_cast<int>(tabCount),
        QuickNavScale(72), QuickNavScale(150));
}

inline void DesktopApp::EnsureNavTabOrder()
{
    std::unordered_set<std::wstring> orderSet(navTabOrder_.begin(), navTabOrder_.end());
    for (const auto& w : widgets_)
    {
        if (w.type == DesktopWidgetType::Collection && !orderSet.count(w.id))
        {
            navTabOrder_.push_back(w.id);
            orderSet.insert(w.id);
        }
    }
    navTabOrder_.erase(
        std::remove_if(navTabOrder_.begin(), navTabOrder_.end(),
            [this](const std::wstring& id) {
                for (const auto& w : widgets_)
                    if (w.type == DesktopWidgetType::Collection && w.id == id) return false;
                return true;
            }),
        navTabOrder_.end());
}

inline RECT DesktopApp::GetQuickNavigationTabRect(const RECT& overlay, size_t tabIndex) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    const size_t tabCount = collectionIndices.size() + 1;
    const int gap = QuickNavScale(8);
    const int available = std::max(1, static_cast<int>(tabs.right - tabs.left));
    int tabWidth = QuickNavScale(92);
    if (tabCount > 0)
        tabWidth = std::clamp((available - gap * static_cast<int>(tabCount - 1)) / static_cast<int>(tabCount),
            QuickNavScale(72), QuickNavScale(150));
    const int left = tabs.left + static_cast<int>(tabIndex) * (tabWidth + gap) - quickNavigationTabScrollOffset_;
    return MakeRect(left, tabs.top, left + tabWidth, tabs.bottom);
}

inline int DesktopApp::GetQuickNavigationColumnCount(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int contentWidth = std::max(1, static_cast<int>(content.right - content.left));
    if (contentWidth < cellW) return 1;
    int columns = contentWidth / cellW;
    if (columns <= 1) return 1;
    int gap = (contentWidth - columns * cellW) / (columns - 1);
    while (columns > 1 && gap < QuickNavScale(8))
    {
        --columns;
        gap = (contentWidth - columns * cellW) / (columns - 1);
    }
    return std::max(1, columns);
}

inline int DesktopApp::GetQuickNavigationGap(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int columns = GetQuickNavigationColumnCount(overlay);
    if (columns <= 0) return 0;
    const int contentWidth = std::max(1, static_cast<int>(content.right - content.left));
    int totalGaps = contentWidth - columns * cellW;
    return totalGaps / columns;
}

inline RECT DesktopApp::GetQuickNavigationItemRect(const RECT& overlay, size_t linearIndex) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int cellH = QuickNavScale(kQuickNavigationCellHeight);
    const int columns = GetQuickNavigationColumnCount(overlay);
    const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    const int gap = GetQuickNavigationGap(overlay);
    int halfPad = gap / 2;
    return MakeRect(
        content.left + halfPad + col * (cellW + gap),
        content.top + row * cellH - quickNavigationScrollOffset_,
        content.left + halfPad + col * (cellW + gap) + cellW,
        content.top + (row + 1) * cellH - quickNavigationScrollOffset_);
}

inline int DesktopApp::GetQuickNavigationMaxScrollOffset(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int columns = GetQuickNavigationColumnCount(overlay);
    const int itemCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int rows = itemCount == 0 ? 1 : (itemCount + columns - 1) / columns;
    const int contentHeight = rows * QuickNavScale(kQuickNavigationCellHeight);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    return std::max(0, contentHeight - visibleHeight);
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

inline void DesktopApp::DrawQuickNavigationOverlay(ID2D1DeviceContext* ctx)
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        return;
    if (!ctx || !quickNavigationOpen_) return;

    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    if (quickNavigationActiveWidgetIndex_ < widgets_.size() &&
        widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::Collection)
    {
        quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
    }

    std::vector<std::wstring> keys = GetQuickNavigationItemKeys();
    quickNavigationRect_ = GetQuickNavigationRect();
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(quickNavigationRect_));
    quickNavigationTabScrollOffset_ = std::clamp(quickNavigationTabScrollOffset_, 0,
        GetQuickNavigationMaxTabScrollOffset(quickNavigationRect_));

    DrawD2DRoundedRectangle(ctx, quickNavigationRect_, static_cast<float>(QuickNavScale(18)),
        D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.94f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.38f), 1.4f);

    RECT titleRect = MakeRect(quickNavigationRect_.left + QuickNavScale(24), quickNavigationRect_.top + QuickNavScale(18),
        quickNavigationRect_.right - QuickNavScale(24), quickNavigationRect_.top + QuickNavScale(46));
    std::wstring title = L"快捷导航";
    if (!keys.empty())
        title += L"  " + std::to_wstring(keys.size()) + L" 项";
    DrawD2DText(ctx, title, titleRect, itemTextFormat_.Get(),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));

    RECT tabs = GetQuickNavigationTabsRect(quickNavigationRect_);
    ctx->PushAxisAlignedClip(ToD2DRect(tabs), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    {
        const size_t tabCount = collectionIndices.size() + 1;
        const int tabWidth = GetQuickNavigationTabWidth();
        const int gap = QuickNavScale(8);

        auto calcTabPosX = [&](size_t tabIdx) -> int {
            return tabs.left + static_cast<int>(tabIdx) * (tabWidth + gap) - quickNavigationTabScrollOffset_;
        };

        int dragTargetTab = -1;
        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            int unit = tabWidth + gap;
            dragTargetTab = static_cast<int>(quickNavTabDragIndex_) + quickNavTabDragDeltaX_ / unit;
            if (dragTargetTab < 1) dragTargetTab = 1;
            if (dragTargetTab > static_cast<int>(collectionIndices.size())) dragTargetTab = static_cast<int>(collectionIndices.size());
        }

        auto drawTab = [&](size_t tab, int offsetX) {
            int posX = calcTabPosX(tab) + offsetX;
            RECT tabRect = MakeRect(posX, tabs.top, posX + tabWidth, tabs.bottom);
            if (tabRect.right <= tabs.left || tabRect.left >= tabs.right) return;

            bool active = tab == 0
                ? quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-1)
                : quickNavigationActiveWidgetIndex_ == collectionIndices[tab - 1];
            bool hovered = false;
            if (!quickNavTabDragging_)
                hovered = PtInRect(&tabRect, lastMousePoint_) != FALSE;

            D2D1_COLOR_F fill, stroke;
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
            {
                fill = D2D1::ColorF(0.30f, 0.36f, 0.44f, 0.96f);
                stroke = D2D1::ColorF(0.47f, 0.55f, 0.71f, 0.9f);
            }
            else
            {
                fill = active
                    ? D2D1::ColorF(0.20f, 0.48f, 0.90f, 0.96f)
                    : hovered
                        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)
                        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
                stroke = D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.28f : 0.14f);
            }
            DrawD2DRoundedRectangle(ctx, tabRect, static_cast<float>(QuickNavScale(7)), fill, stroke, 1.0f);

            std::wstring label = L"全部";
            if (tab > 0)
            {
                const DesktopWidget& widget = widgets_[collectionIndices[tab - 1]];
                label = widget.title.empty() ? L"集合" + std::to_wstring(tab) : widget.title;
            }
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DText(ctx, label, textRect,
                navTabTextFormat_ ? navTabTextFormat_.Get() : itemTextFormat_.Get(),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 1.0f : 0.88f));
        };

        for (size_t tab = 0; tab < tabCount; ++tab)
        {
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
                continue;
            int offsetX = 0;
            if (quickNavTabDragging_ && dragTargetTab >= 1)
            {
                int src = static_cast<int>(quickNavTabDragIndex_);
                int cur = static_cast<int>(tab);
                if (cur > src && cur <= dragTargetTab) offsetX = -(tabWidth + gap);
                else if (cur < src && cur >= dragTargetTab) offsetX = tabWidth + gap;
            }
            drawTab(tab, offsetX);
        }

        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            int posX = calcTabPosX(quickNavTabDragIndex_) + quickNavTabDragDeltaX_;
            RECT tabRect = MakeRect(posX, tabs.top, posX + tabWidth, tabs.bottom);

            std::wstring label = L"全部";
            if (quickNavTabDragIndex_ > 0)
            {
                const DesktopWidget& widget = widgets_[collectionIndices[quickNavTabDragIndex_ - 1]];
                label = widget.title.empty() ? L"集合" + std::to_wstring(quickNavTabDragIndex_) : widget.title;
            }
            DrawD2DRoundedRectangle(ctx, tabRect, static_cast<float>(QuickNavScale(7)),
                D2D1::ColorF(0.24f, 0.31f, 0.43f, 0.96f),
                D2D1::ColorF(0.39f, 0.51f, 0.78f, 0.9f), 1.0f);
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DText(ctx, label, textRect,
                navTabTextFormat_ ? navTabTextFormat_.Get() : itemTextFormat_.Get(),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));

            if (dragTargetTab >= 1 && static_cast<size_t>(dragTargetTab) <= collectionIndices.size())
            {
                int insertX = calcTabPosX(static_cast<size_t>(dragTargetTab)) - gap / 2;
                RECT indicatorRect = MakeRect(insertX - 1, tabs.top + QuickNavScale(4), insertX + 1, tabs.bottom - QuickNavScale(4));
                DrawD2DFilledRectangle(ctx, indicatorRect, D2D1::ColorF(0.32f, 0.55f, 0.92f, 0.9f), D2D1::ColorF(0.32f, 0.55f, 0.92f, 0.9f));
            }
        }
    }

    ctx->PopAxisAlignedClip();

    RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
    ctx->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (keys.empty())
    {
        RECT emptyRect = content;
        emptyRect.top += QuickNavScale(28);
        DrawD2DText(ctx, collectionIndices.empty() ? L"暂无集合组件" : L"当前分类暂无项目",
            emptyRect, itemTextFormat_.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.58f));
    }
    else
    {
        for (size_t i = 0; i < keys.size(); ++i)
        {
            RECT itemRect = GetQuickNavigationItemRect(quickNavigationRect_, i);
            if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;

            size_t itemIndex = FindItemIndexByKey(keys[i]);
            if (itemIndex == static_cast<size_t>(-1)) continue;

            bool hovered = PtInRect(&itemRect, lastMousePoint_) != FALSE;
            DesktopItem& item = items_[itemIndex];

            if (hovered)
            {
                DrawD2DRoundedRectangle(ctx, itemRect, 6.0f,
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
            }

            const int cellW = itemRect.right - itemRect.left;
            const int cellH = itemRect.bottom - itemRect.top;
            const int qnTextH = std::max(1, QuickNavScale(kQuickNavigationTextHeight));
            const int inset = QuickNavScale(2);
            const int minIcon = QuickNavScale(16);
            const int maxIconW = std::max(minIcon, cellW - inset * 2);
            const int maxIconH = std::max(minIcon, cellH - qnTextH - inset * 2);
            const int iconSz = std::min(maxIconW, maxIconH);
            const int iconX = itemRect.left + (cellW - iconSz) / 2;
            const int iconY = itemRect.top + inset;
            RECT qnIconRect = MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
            RECT qnTextRect = MakeRect(itemRect.left + QuickNavScale(4),
                iconY + iconSz + QuickNavScale(2),
                itemRect.right - QuickNavScale(4),
                iconY + iconSz + QuickNavScale(2) + qnTextH);

            if (item.iconState == IconState::Loading)
            {
                DrawPlaceholderIcon(ctx, item.sysIconIndex, qnIconRect, 1.0f);
            }
            else if (item.iconBitmap)
            {
                ID2D1Bitmap1* bmp = GetOrCreateD2DBitmap(item.iconBitmap);
                if (bmp)
                {
                    D2D1_RECT_F dst = D2D1::RectF(
                        static_cast<float>(qnIconRect.left), static_cast<float>(qnIconRect.top),
                        static_cast<float>(qnIconRect.right), static_cast<float>(qnIconRect.bottom));
                    ctx->DrawBitmap(bmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                }
            }

            if (item.shortcutArrow && item.iconState != IconState::Loading)
                DrawShortcutArrowOverlay(ctx, qnIconRect, 1.0f);

            if (!item.name.empty())
            {
                const float tw = static_cast<float>(qnTextRect.right - qnTextRect.left);
                const float th = static_cast<float>(qnTextRect.bottom - qnTextRect.top);
                ComPtr<IDWriteTextLayout> layout;
                if (tw > 0 && th > 0 &&
                    SUCCEEDED(dwriteFactory_->CreateTextLayout(
                        item.name.c_str(), static_cast<UINT32>(item.name.size()),
                        itemTextFormat_.Get(), tw, th, &layout)) && layout)
                {
                    const DWRITE_TEXT_RANGE all = { 0, static_cast<UINT32>(item.name.size()) };
                    layout->SetFontSize(itemFontSize_, all);
                    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                        itemFontSize_ * 7.0f / 6.0f, itemFontSize_ * 5.0f / 6.0f);
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
                    const D2D1_COLOR_F textColor(1.0f, 1.0f, 1.0f, 0.96f);
                    const auto brushKey = D2DColorBrushKey(textColor);
                    auto bit = brushCache_.find(brushKey);
                    if (bit == brushCache_.end())
                    {
                        ComPtr<ID2D1SolidColorBrush> br;
                        if (SUCCEEDED(ctx->CreateSolidColorBrush(textColor, &br)) && br)
                            bit = brushCache_.emplace(brushKey, std::move(br)).first;
                    }
                    if (bit != brushCache_.end())
                        ctx->DrawTextLayout(
                            D2D1::Point2F(static_cast<float>(qnTextRect.left),
                                static_cast<float>(qnTextRect.top)),
                            layout.Get(), bit->second.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    }
    ctx->PopAxisAlignedClip();

    const int columns = GetQuickNavigationColumnCount(quickNavigationRect_);
    const int rows = keys.empty() ? 1 : (static_cast<int>(keys.size()) + columns - 1) / columns;
    const int contentHeight = rows * QuickNavScale(kQuickNavigationCellHeight);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    const bool hovered = PtInRect(&quickNavigationRect_, lastMousePoint_) != FALSE;
    DrawScrollbarAt(ctx, content, contentHeight, visibleHeight, quickNavigationScrollOffset_, hovered);
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
    brushCache_.clear();
    if (dragSession_.IsActive())
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        UINT w = std::max<LONG>(1, client.right - client.left);
        UINT h = std::max<LONG>(1, client.bottom - client.top);

        bool cacheReady = dragRenderCache_.Ensure(d2dDevice_.Get(), D2D1_SIZE_U{ w, h },
            dragSession_.StaticSceneRevision(),
            [&](ID2D1DeviceContext* cacheCtx) { DrawStaticBackground(cacheCtx); });
        brushCache_.clear();
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

inline void DesktopApp::DrawShortcutArrowOverlay(ID2D1DeviceContext* ctx, RECT iconRect, float alpha)
{
    if (!ctx) return;

    if (!shortcutArrowBitmap_)
    {
        SHSTOCKICONINFO sii{};
        sii.cbSize = sizeof(sii);
        if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) || !sii.hIcon)
            return;

        int w = GetSystemMetrics(SM_CXICON);
        int h = GetSystemMetrics(SM_CYICON);
        if (w <= 0) w = 32;
        if (h <= 0) h = 32;

        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);

        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi);
        bi.biWidth = w;
        bi.biHeight = -h;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, nullptr, nullptr, 0);
        if (!dib)
        {
            DeleteDC(memDc);
            ReleaseDC(nullptr, screenDc);
            DestroyIcon(sii.hIcon);
            return;
        }

        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDc, dib));
        DrawIconEx(memDc, 0, 0, sii.hIcon, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(memDc, oldBmp);

        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        DIBSECTION ds{};
        GetObjectW(dib, sizeof(ds), &ds);
        ComPtr<ID2D1Bitmap1> d2dBmp;
        if (SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &d2dBmp)))
        {
            D2D1_RECT_U srcRect = D2D1::RectU(0, 0, static_cast<UINT32>(w), static_cast<UINT32>(h));
            d2dBmp->CopyFromMemory(&srcRect, ds.dsBm.bmBits, ds.dsBm.bmWidthBytes);
            shortcutArrowBitmap_ = std::move(d2dBmp);
            shortcutArrowBitmapSize_ = { w, h };
        }

        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        DestroyIcon(sii.hIcon);
    }

    if (!shortcutArrowBitmap_) return;

    float scale = static_cast<float>(iconRect.bottom - iconRect.top) / 64.0f;
    int arrowSz = static_cast<int>(30.0f * scale + 0.5f);
    if (arrowSz < 10) arrowSz = 10;
    int arrowX = iconRect.left;
    int arrowY = iconRect.bottom - arrowSz;

    D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(arrowX),
        static_cast<float>(arrowY),
        static_cast<float>(arrowX + arrowSz),
        static_cast<float>(arrowY + arrowSz));

    ctx->DrawBitmap(shortcutArrowBitmap_.Get(), dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
}

inline void DesktopApp::CacheSystemImageListSmall()
{
    if (systemIconStripBitmap_) return;
    HIMAGELIST himl = nullptr;
    if (FAILED(SHGetImageList(SHIL_SMALL, IID_IImageList, reinterpret_cast<void**>(&himl))) || !himl)
        return;
    systemImageListSmall_ = himl;
    int cx = 0, cy = 0, count = 0;
    IImageList* imgList = reinterpret_cast<IImageList*>(himl);
    imgList->GetIconSize(&cx, &cy);
    imgList->GetImageCount(&count);
    if (cx <= 0 || cy <= 0 || count <= 0) return;
    systemIconStripCount_ = count;
    systemIconStripIconSize_ = { cx, cy };

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    int totalWidth = cx * count;
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = totalWidth;
    bi.biHeight = -cy;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, nullptr, nullptr, 0);
    if (!dib) { DeleteDC(memDc); ReleaseDC(nullptr, screenDc); return; }

    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDc, dib));
    for (int i = 0; i < count; ++i)
        ImageList_Draw(himl, i, memDc, i * cx, 0, ILD_TRANSPARENT | ILD_PRESERVEALPHA);
    SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    DIBSECTION ds{};
    GetObjectW(dib, sizeof(ds), &ds);
    ComPtr<ID2D1Bitmap1> d2dBmp;
    if (SUCCEEDED(d2dContext_->CreateBitmap(D2D1::SizeU(totalWidth, cy), nullptr, 0, &props, &d2dBmp)))
    {
        D2D1_RECT_U srcRect = D2D1::RectU(0, 0, static_cast<UINT32>(totalWidth), static_cast<UINT32>(cy));
        d2dBmp->CopyFromMemory(&srcRect, ds.dsBm.bmBits, ds.dsBm.bmWidthBytes);
        systemIconStripBitmap_ = std::move(d2dBmp);
    }
    DeleteObject(dib);
}

inline void DesktopApp::DrawPlaceholderIcon(ID2D1DeviceContext* ctx, int sysIconIndex, RECT iconRect, float alpha)
{
    if (!ctx || sysIconIndex < 0) return;
    if (!systemIconStripBitmap_) CacheSystemImageListSmall();
    if (!systemIconStripBitmap_ || sysIconIndex >= systemIconStripCount_) return;

    int cx = systemIconStripIconSize_.cx;
    int srcX = sysIconIndex * cx;

    D2D1_RECT_F src = D2D1::RectF(static_cast<float>(srcX), 0.0f,
        static_cast<float>(srcX + cx), static_cast<float>(systemIconStripIconSize_.cy));
    D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
        static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
    ctx->DrawBitmap(systemIconStripBitmap_.Get(), dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR, &src);
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
