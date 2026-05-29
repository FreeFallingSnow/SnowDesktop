#pragma once
// Inline implementations for SnowDesktopAppOO — Graphics & Rendering.
// This file is included by app_oo.h after the class definition.

// ── Graphics ─────────────────────────────────────────────────

inline bool SnowDesktopAppOO::InitGraphics()
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

    return true;
}

inline HRESULT SnowDesktopAppOO::CreateOrResizeCompositionSurface()
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

inline void SnowDesktopAppOO::OnPaint()
    {
        auto L = [](const wchar_t* s) {
            HANDLE f = CreateFileW(L"SnowDesktopOO_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, s, static_cast<DWORD>(wcslen(s)*2), &w, nullptr);
                WriteFile(f, L"\r\n", 4, &w, nullptr); CloseHandle(f); }
        };
        L(L"OnPaint");
        if (FAILED(CreateOrResizeCompositionSurface())) { L(L"CreateSurface FAILED"); return; }

        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        HRESULT hr = dcompSurface_->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&rawContext), &updateOffset);
        if (FAILED(hr)) { L(L"BeginDraw FAILED"); return; }

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

inline D2D1_RECT_F SnowDesktopAppOO::ToD2DRect(const RECT& r)
{
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
        static_cast<float>(r.right), static_cast<float>(r.bottom));
}

inline RECT SnowDesktopAppOO::GetItemIconRect(RECT bounds) const
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

inline RECT SnowDesktopAppOO::GetItemTextRect(RECT bounds, bool expanded) const
{
    RECT iconRect = GetItemIconRect(bounds);
    const int textTop = iconRect.bottom + 2;
    const int textH = expanded ? kTextExpandedHeight : kTextCollapsedHeight;
    return MakeRect(bounds.left + 4, textTop, bounds.right - 4, textTop + textH);
}

inline RECT SnowDesktopAppOO::GetItemSelectionRect(RECT bounds, bool expanded) const
{
    RECT textRect = GetItemTextRect(bounds, expanded);
    RECT selection = UnionCopy(GetItemIconRect(bounds), textRect);
    selection.left = std::max(bounds.left + 3, selection.left - 4);
    selection.top = std::max(bounds.top, selection.top - 2);
    selection.right = std::min(bounds.right - 3, selection.right + 4);
    selection.bottom = std::min(bounds.bottom - 2, textRect.bottom);
    return selection;
}

inline void SnowDesktopAppOO::DrawD2DRoundedRectangle(ID2D1DeviceContext* ctx, RECT rect, float radius,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(ToD2DRect(rect), radius, radius);
    ComPtr<ID2D1SolidColorBrush> fillBrush;
    if (fill.a > 0.0f && SUCCEEDED(ctx->CreateSolidColorBrush(fill, &fillBrush)) && fillBrush)
        ctx->FillRoundedRectangle(rounded, fillBrush.Get());
    ComPtr<ID2D1SolidColorBrush> strokeBrush;
    if (stroke.a > 0.0f && SUCCEEDED(ctx->CreateSolidColorBrush(stroke, &strokeBrush)) && strokeBrush)
        ctx->DrawRoundedRectangle(rounded, strokeBrush.Get(), strokeWidth, nullptr);
}

inline void SnowDesktopAppOO::DrawD2DFilledRectangle(ID2D1DeviceContext* ctx, RECT rect,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke)
{
    ComPtr<ID2D1SolidColorBrush> fillBrush;
    if (SUCCEEDED(ctx->CreateSolidColorBrush(fill, &fillBrush)) && fillBrush)
        ctx->FillRectangle(ToD2DRect(rect), fillBrush.Get());
    ComPtr<ID2D1SolidColorBrush> strokeBrush;
    if (SUCCEEDED(ctx->CreateSolidColorBrush(stroke, &strokeBrush)) && strokeBrush)
        ctx->DrawRectangle(ToD2DRect(rect), strokeBrush.Get(), 1.0f, nullptr);
}

extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);

inline void SnowDesktopAppOO::RenderFrame(ID2D1DeviceContext* ctx)
{

    for (auto& item : items_)
    {
        if (IsRectEmptyRect(item.bounds)) continue;
        if (draggingItems_ && item.selected && !dragCopyMode_ && !dragLinkMode_) continue;
        RECT bounds = item.bounds;

        const bool hovered = PtInRect(&bounds, lastMousePoint_) != FALSE;
        const float opacity = item.isCut ? 0.4f : 1.0f;

        if (hovered && !item.selected)
        {
            DrawD2DRoundedRectangle(ctx, bounds, 6.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f * opacity),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f * opacity));
        }

        RECT iconRect = GetItemIconRect(bounds);

        if (item.selected)
        {
            DrawD2DFilledRectangle(ctx,
                GetItemSelectionRect(bounds, true),
                D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.34f * opacity),
                D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.55f * opacity));
        }

        if (item.iconBitmap)
        {
            ID2D1Bitmap1* bmp = GetOrCreateD2DBitmap(item.iconBitmap);
            if (bmp)
            {
                D2D1_RECT_F dst = D2D1::RectF(
                    static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                    static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
                ctx->DrawBitmap(bmp, dst, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
            }
        }

        if (dwriteFactory_ && itemTextFormat_ && !item.name.empty())
        {
            RECT textRect = GetItemTextRect(bounds, item.selected);
            float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
            float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                item.name.c_str(), static_cast<UINT32>(item.name.size()),
                itemTextFormat_.Get(), tw, th, &layout)) && layout)
            {
                ComPtr<ID2D1SolidColorBrush> shadowBrush;
                ComPtr<ID2D1SolidColorBrush> textBrush;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f * opacity), &shadowBrush);
                ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity), &textBrush);

                const float tx = static_cast<float>(textRect.left);
                const float ty = static_cast<float>(textRect.top);
                if (shadowBrush)
                {
                    const D2D1_POINT_2F offsets[] = {
                        { tx - 1.0f, ty }, { tx + 1.0f, ty },
                        { tx, ty - 1.0f }, { tx, ty + 1.0f },
                    };
                    for (auto& pt : offsets)
                        ctx->DrawTextLayout(pt, layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                if (textBrush)
                    ctx->DrawTextLayout(D2D1::Point2F(tx, ty), layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    // Draw drag target indicator or hand-off highlight
    if (draggingItems_ || externalDragActive_)
    {
        POINT dragPoint = draggingItems_ ? dragCurrentPoint_ : externalDragPoint_;
        bool showHandoff = false;
        int hitUnderCursor = HitTestItem(dragPoint);
        if (hitUnderCursor >= 0)
        {
            if (draggingItems_ && items_[hitUnderCursor].selected)
            {
                // skip
            }
            else
            {
                RECT iconRect = GetItemIconRect(items_[hitUnderCursor].bounds);
                if (PtInRect(&iconRect, dragPoint))
                {
                    showHandoff = true;
                    DrawD2DRoundedRectangle(ctx, items_[hitUnderCursor].bounds, 6.0f,
                        D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.15f),
                        D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.60f), 2.0f);
                }
            }
        }
        if (!showHandoff)
        {
            GridCell targetCell = draggingItems_
                ? FindBestDropCell(CellFromPoint(GetDragTargetPoint(dragPoint)))
                : CellFromPoint(dragPoint);
            if (!targetCell.pageId.empty())
            {
                if (draggingItems_)
                {
                    std::vector<PendingGridMove> moves = BuildSelectedMove(targetCell);
                    if (!moves.empty())
                    {
                        for (const auto& move : moves)
                        {
                            RECT targetRect = GetGridRect(gridPages_, move.cell, items_[move.index].gridSpan);
                            DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
                                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
                                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
                        }
                    }
                }
                else
                {
                    extern inline RECT GetGridRect(const std::vector<GridPage>&, const GridCell&, GridSpan);
                    extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
                    int previewCount = std::max(1, static_cast<int>(externalDropFileCount_));
                    const GridPage* targetPage = FindGridPage(gridPages_, targetCell.pageId);
                    int cols = targetPage ? targetPage->columns : 1;
                    int row = targetCell.row;
                    int col = targetCell.column;
                    for (int i = 0; i < previewCount; ++i)
                    {
                        GridCell previewCell{ targetCell.pageId, col, row };
                        RECT targetRect = GetGridRect(gridPages_, previewCell, GridSpan{1, 1});
                        DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
                            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
                            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
                        ++col;
                        if (col >= cols) { col = 0; ++row; }
                    }
                }
            }
        }
    }

    // Draw dragged items at offset
    if (draggingItems_)
    {
        int dx = dragCurrentPoint_.x - mouseDownPoint_.x;
        int dy = dragCurrentPoint_.y - mouseDownPoint_.y;

        for (auto& item : items_)
        {
            if (!item.selected) continue;
            if (IsRectEmptyRect(item.bounds)) continue;
            RECT bounds = item.bounds;
            RECT draggedBounds = MakeRect(bounds.left + dx, bounds.top + dy,
                bounds.right + dx, bounds.bottom + dy);

            RECT iconRect = GetItemIconRect(draggedBounds);

            DrawD2DFilledRectangle(ctx,
                GetItemSelectionRect(draggedBounds, true),
                D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.28f),
                D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.45f));

            if (item.iconBitmap)
            {
                ID2D1Bitmap1* bmp = GetOrCreateD2DBitmap(item.iconBitmap);
                if (bmp)
                {
                    D2D1_RECT_F dst = D2D1::RectF(
                        static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                        static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
                    ctx->DrawBitmap(bmp, dst, 0.7f, D2D1_INTERPOLATION_MODE_LINEAR);
                }
            }

            if (dwriteFactory_ && itemTextFormat_ && !item.name.empty())
            {
                RECT textRect = GetItemTextRect(draggedBounds, true);
                float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
                float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    item.name.c_str(), static_cast<UINT32>(item.name.size()),
                    itemTextFormat_.Get(), tw, th, &layout)) && layout)
                {
                    ComPtr<ID2D1SolidColorBrush> textBrush;
                    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.7f), &textBrush);
                    if (textBrush)
                        ctx->DrawTextLayout(D2D1::Point2F(
                            static_cast<float>(textRect.left), static_cast<float>(textRect.top)),
                            layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
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

inline void SnowDesktopAppOO::GetNavButtonRects(RECT& outPrev, RECT& outNext) const
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

inline void SnowDesktopAppOO::DrawPageNavButtons(ID2D1DeviceContext* ctx)
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

    bool dragging = draggingItems_;
    bool hoverPrev = (navHoverSide_ == -1);
    bool hoverNext = (navHoverSide_ == 1);

    if (hasPrev) drawArrow(prevRect, L"\u25C0", dragging || hoverPrev, hoverPrev);
    if (hasNext) drawArrow(nextRect, L"\u25B6", dragging || hoverNext, hoverNext);
}

