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
    dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"", &itemTextFormat_);
    if (itemTextFormat_)
    {
        itemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        itemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        itemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        itemTextFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f, 13.0f);
    }

    dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &listItemTextFormat_);
    if (listItemTextFormat_)
    {
        listItemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        listItemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        listItemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
            auto L = [](const wchar_t* s) {
                HANDLE f = CreateFileW(L"SnowDesktopOO_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, s, static_cast<DWORD>(wcslen(s)*2), &w, nullptr);
                    WriteFile(f, L"\r\n", 4, &w, nullptr); CloseHandle(f); }
            };
            L(buf);
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

inline RECT DesktopApp::GetItemIconRect(RECT bounds) const
{
    const int cellW = bounds.right - bounds.left;
    const int cellH = bounds.bottom - bounds.top;
    if (cellH < 50)
    {
        const int iconSz = std::min(32, cellH - 4);
        return MakeRect(
            bounds.left + 4,
            bounds.top + (cellH - iconSz) / 2,
            bounds.left + 4 + iconSz,
            bounds.top + (cellH + iconSz) / 2);
    }
    const int maxIconW = std::max(16, cellW - 8);
    const int maxIconH = std::max(16, cellH - kTextHeight - 8);
    const int iconSz = std::min(maxIconW, maxIconH);
    const int iconX = bounds.left + (cellW - iconSz) / 2;
    const int iconY = bounds.top + 2;
    return MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
}

inline RECT DesktopApp::GetItemTextRect(RECT bounds, bool expanded) const
{
    RECT iconRect = GetItemIconRect(bounds);
    const int textTop = iconRect.bottom + 2;
    const int textH = expanded ? kTextExpandedHeight : kTextCollapsedHeight;
    return MakeRect(bounds.left + 4, textTop, bounds.right - 4, textTop + textH);
}

inline RECT DesktopApp::GetItemSelectionRect(RECT bounds, bool expanded) const
{
    RECT textRect = GetItemTextRect(bounds, expanded);
    RECT selection = UnionCopy(GetItemIconRect(bounds), textRect);
    selection.left = std::max(bounds.left + 3, selection.left - 4);
    selection.top = std::max(bounds.top, selection.top - 2);
    selection.right = std::min(bounds.right - 3, selection.right + 4);
    selection.bottom = std::min(bounds.bottom - 2, textRect.bottom);
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

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()),
        itemTextFormat_.Get(), tw, th, &layout)) || !layout)
        return;

    ComPtr<ID2D1SolidColorBrush> shadowBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f * opacity), &shadowBrush);
    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity), &textBrush);

    const float tx = static_cast<float>(textRect.left);
    const float ty = static_cast<float>(textRect.top);
    if (shadowBrush)
    {
        const D2D1_POINT_2F offsets[] = {
            { tx - 1.0f, ty }, { tx + 1.0f, ty },
            { tx, ty - 1.0f }, { tx, ty + 1.0f },
        };
        for (auto& pt : offsets)
            context->DrawTextLayout(pt, layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (textBrush)
        context->DrawTextLayout(D2D1::Point2F(tx, ty), layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline void DesktopApp::DrawD2DText(ID2D1DeviceContext* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color)
{
    if (!ctx || !format || text.empty() || IsRectEmptyRect(rect)) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(ctx->CreateSolidColorBrush(color, &brush)) && brush)
    {
        ctx->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
            ToD2DRect(rect), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
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
        D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.92f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.38f), 1.4f);

    RECT titleRect = MakeRect(popupRect_.left + 22, popupRect_.top + 18,
        popupRect_.right - 22, popupRect_.top + 44);
    std::wstring title = widget.title.empty() ? L"集合" : widget.title;
    DrawD2DText(ctx, title, titleRect, itemTextFormat_.Get(),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));

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
    for (auto& ooItem : items_oo_)
    {
        auto* icon = dynamic_cast<DesktopIcon*>(ooItem.get());
        if (!icon) continue;
        DesktopItem* di = icon->GetDesktopItem();
        if (!di || IsRectEmptyRect(di->bounds)) continue;
        if (dragSession_.IsActive() && !dragSession_.Items().empty() &&
            dragSession_.IsMoveAction() && di->selected)
            continue;

        const bool hovered = PtInRect(&di->bounds, lastMousePoint_) != FALSE;
        const bool selected = di->selected;
        int state = selected ? 2 : (hovered ? 1 : 0);
        icon->Draw(ctx, di->bounds, state);
    }

    // Widget chrome
    for (auto& c : containers_)
    {
        if (widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc)
            {
                auto* wd = wc->GetWidgetData();
                if (wd && mouseDownWidgetIndex_ < widgets_.size()
                    && wd == &widgets_[mouseDownWidgetIndex_])
                    continue;
            }
        }
        c->DrawChrome(ctx, lastMousePoint_);
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
        DrawD2DFilledRectangle(ctx, marqueeRect_,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
            D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
    }

    DrawPageNavButtons(ctx);
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
    if (!targetPage) targetPage = &gridPages_.back();

    constexpr LONG buttonW = 40, buttonH = 72, padX = 4;
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
    if (MaxPageOffset() <= 0 && pageOffset_ <= 0) return;
    if (gridPages_.empty()) return;

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();
    if (!hasPrev && !hasNext) return;

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    auto drawArrow = [&](const RECT& rect, const std::wstring& arrow, bool visible, bool hovered) {
        if (!visible) return;

        D2D1_RECT_F d2dRect = D2D1::RectF(
            static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right), static_cast<float>(rect.bottom));

        float bgAlpha = hovered ? 1.0f : 0.85f;
        ComPtr<ID2D1SolidColorBrush> bg;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, bgAlpha), &bg);
        if (bg) ctx->FillRoundedRectangle(
            D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), bg.Get());

        ComPtr<ID2D1SolidColorBrush> borderBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f), &borderBrush);
        if (borderBrush) ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), borderBrush.Get(), 1.0f);

        float textAlpha = hovered ? 1.0f : 0.65f;
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

    bool dragging = dragSession_.IsActive() && !dragSession_.Items().empty();
    bool hoverPrev = (navHoverSide_ == -1);
    bool hoverNext = (navHoverSide_ == 1);

    if (hasPrev) drawArrow(prevRect, L"\u25C0", dragging || hoverPrev, hoverPrev);
    if (hasNext) drawArrow(nextRect, L"\u25B6", dragging || hoverNext, hoverNext);
}
