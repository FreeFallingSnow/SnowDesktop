#include "app.h"
#include <commoncontrols.h>

// Shell-icon decoration, privacy placeholders and quick-navigation icons.

void DesktopApp::DrawShortcutArrowOverlay(ID2D1RenderTarget* ctx, RECT iconRect, float alpha)
{
    if (!ctx) return;

    if (iconBeautifyEnabled_)
    {
        const int iconHeight = std::max(1, static_cast<int>(iconRect.bottom - iconRect.top));
        const float scale = static_cast<float>(iconHeight) / 64.0f;
        const int pad = std::max(1, static_cast<int>(std::round(2.0f * scale)));
        int badgeSz = static_cast<int>(std::round(17.0f * scale));
        const int iconWidth = std::max(1, static_cast<int>(iconRect.right - iconRect.left));
        badgeSz = std::clamp(badgeSz, 9, std::max(9, iconWidth));
        RECT badgeRect = MakeRect(
            iconRect.left + pad,
            iconRect.bottom - badgeSz - pad,
            iconRect.left + pad + badgeSz,
            iconRect.bottom - pad);

        ComPtr<ID2D1SolidColorBrush> badgeFillBrush;
        ComPtr<ID2D1SolidColorBrush> badgeStrokeBrush;
        if (FAILED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.86f, 0.89f, 0.94f, 0.96f * alpha), &badgeFillBrush)) || !badgeFillBrush ||
            FAILED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.54f, 0.61f, 0.72f, 0.58f * alpha), &badgeStrokeBrush)) || !badgeStrokeBrush)
        {
            return;
        }

        const float left = static_cast<float>(badgeRect.left);
        const float top = static_cast<float>(badgeRect.top);
        const float right = static_cast<float>(badgeRect.right);
        const float bottom = static_cast<float>(badgeRect.bottom);
        const float sz = right - left;
        const D2D1_ELLIPSE badgeEllipse = D2D1::Ellipse(
            D2D1::Point2F((left + right) * 0.5f, (top + bottom) * 0.5f),
            sz * 0.5f,
            sz * 0.5f);

        ComPtr<ID2D1SolidColorBrush> arrowBrush;
        if (FAILED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.18f, 0.30f, 0.48f, 0.92f * alpha), &arrowBrush)) || !arrowBrush)
        {
            return;
        }

        ctx->FillEllipse(badgeEllipse, badgeFillBrush.Get());
        ctx->DrawEllipse(badgeEllipse, badgeStrokeBrush.Get(), std::max(1.0f, 1.1f * scale));

        const float stroke = std::max(1.0f, sz * 0.11f);
        const D2D1_POINT_2F start = D2D1::Point2F(left + sz * 0.30f, top + sz * 0.70f);
        const D2D1_POINT_2F end = D2D1::Point2F(left + sz * 0.70f, top + sz * 0.30f);
        ctx->DrawLine(start, end, arrowBrush.Get(), stroke);
        ctx->DrawLine(end, D2D1::Point2F(left + sz * 0.46f, top + sz * 0.30f), arrowBrush.Get(), stroke);
        ctx->DrawLine(end, D2D1::Point2F(left + sz * 0.70f, top + sz * 0.54f), arrowBrush.Get(), stroke);
        return;
    }

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

float DesktopApp::GetBeautifiedIconCornerRadius(int width, int height)
{
    return std::max(6.0f,
        static_cast<float>(std::min(width, height)) * kIconBeautifyCornerRadiusRatio);
}

void DesktopApp::DrawBeautifiedIconPlate(ID2D1RenderTarget* ctx, RECT rect,
    D2D1_COLOR_F fill, D2D1_COLOR_F border, float strokeWidth)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    ComPtr<ID2D1Factory> factory;
    ctx->GetFactory(&factory);
    if (!factory) return;

    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry ||
        FAILED(geometry->Open(&sink)) || !sink)
        return;

    const float left = static_cast<float>(rect.left);
    const float top = static_cast<float>(rect.top);
    const float right = static_cast<float>(rect.right);
    const float bottom = static_cast<float>(rect.bottom);
    const float radius = std::min(GetBeautifiedIconCornerRadius(
        rect.right - rect.left, rect.bottom - rect.top),
        std::min(right - left, bottom - top) * 0.5f);
    constexpr int kCornerSegments = 12;
    const float coordinatePower = 2.0f / kIconBeautifyCornerExponent;
    auto cornerPoint = [&](float centerX, float centerY, float angle) {
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const float x = std::copysign(std::pow(std::abs(cosine), coordinatePower), cosine);
        const float y = std::copysign(std::pow(std::abs(sine), coordinatePower), sine);
        return D2D1::Point2F(centerX + radius * x, centerY + radius * y);
    };
    auto addCorner = [&](float centerX, float centerY, float startAngle, float endAngle) {
        for (int i = 1; i <= kCornerSegments; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(kCornerSegments);
            sink->AddLine(cornerPoint(centerX, centerY,
                startAngle + (endAngle - startAngle) * t));
        }
    };

    constexpr float kPi = 3.14159265358979323846f;
    sink->BeginFigure(D2D1::Point2F(left + radius, top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(right - radius, top));
    addCorner(right - radius, top + radius, -kPi * 0.5f, 0.0f);
    sink->AddLine(D2D1::Point2F(right, bottom - radius));
    addCorner(right - radius, bottom - radius, 0.0f, kPi * 0.5f);
    sink->AddLine(D2D1::Point2F(left + radius, bottom));
    addCorner(left + radius, bottom - radius, kPi * 0.5f, kPi);
    sink->AddLine(D2D1::Point2F(left, top + radius));
    addCorner(left + radius, top + radius, kPi, kPi * 1.5f);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;

    ComPtr<ID2D1SolidColorBrush> fillBrush;
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    if (SUCCEEDED(ctx->CreateSolidColorBrush(fill, &fillBrush)) && fillBrush)
        ctx->FillGeometry(geometry.Get(), fillBrush.Get());
    if (strokeWidth > 0.0f && border.a > 0.0f &&
        SUCCEEDED(ctx->CreateSolidColorBrush(border, &borderBrush)) && borderBrush)
        ctx->DrawGeometry(geometry.Get(), borderBrush.Get(), strokeWidth);
}

void DesktopApp::DrawPrivacyFaIcon(
    ID2D1DeviceContext* ctx, RECT rect, bool directory)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    ComPtr<ID2D1Bitmap1>& cached = directory
        ? privacyFolderIconBitmap_ : privacyFileIconBitmap_;
    if (!cached)
    {
        constexpr int bitmapSize = kIconBitmapSize;
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = bitmapSize;
        bitmapInfo.bmiHeader.biHeight = -bitmapSize;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        HDC screenDc = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP source = CreateDIBSection(screenDc, &bitmapInfo,
            DIB_RGB_COLORS, &bits, nullptr, 0);
        if (source && bits)
        {
            std::fill_n(static_cast<std::uint32_t*>(bits),
                bitmapSize * bitmapSize, 0u);
            HDC memoryDc = CreateCompatibleDC(screenDc);
            if (memoryDc)
            {
                HGDIOBJ oldBitmap = SelectObject(memoryDc, source);
                HFONT font = CreateFontW(-static_cast<int>(bitmapSize * 0.68f),
                    0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    L"Font Awesome 6 Free Solid");
                HGDIOBJ oldFont = font ? SelectObject(memoryDc, font) : nullptr;
                SetBkMode(memoryDc, TRANSPARENT);
                SetTextColor(memoryDc, RGB(255, 255, 255));
                RECT glyphRect{ 0, 0, bitmapSize, bitmapSize };
                const wchar_t* glyph = L"";
                DrawTextW(memoryDc, glyph, -1, &glyphRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                if (oldFont) SelectObject(memoryDc, oldFont);
                if (font) DeleteObject(font);
                SelectObject(memoryDc, oldBitmap);
                DeleteDC(memoryDc);

                constexpr int glyphR = 0xff;
                constexpr int glyphG = 0xdb;
                constexpr int glyphB = 0x76;
                auto* pixels = static_cast<std::uint32_t*>(bits);
                for (int i = 0; i < bitmapSize * bitmapSize; ++i)
                {
                    const std::uint32_t pixel = pixels[i];
                    const int alpha = static_cast<int>(std::max({
                        pixel & 0xffu, (pixel >> 8) & 0xffu,
                        (pixel >> 16) & 0xffu }));
                    pixels[i] = static_cast<std::uint32_t>(alpha) << 24 |
                        static_cast<std::uint32_t>((glyphR * alpha + 127) / 255) << 16 |
                        static_cast<std::uint32_t>((glyphG * alpha + 127) / 255) << 8 |
                        static_cast<std::uint32_t>((glyphB * alpha + 127) / 255);
                }
                cached = CreateD2DBitmapFromHBitmap(source, true);
            }
            DeleteObject(source);
        }
        ReleaseDC(nullptr, screenDc);
    }

    if (cached)
    {
        ctx->DrawBitmap(cached.Get(), ToD2DRect(rect), 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR);
        return;
    }

    const float luminance = iconBeautifyBgStartR_ * 0.2126f +
        iconBeautifyBgStartG_ * 0.7152f + iconBeautifyBgStartB_ * 0.0722f;
    const D2D1_COLOR_F fill = D2D1::ColorF(iconBeautifyBgStartR_,
        iconBeautifyBgStartG_, iconBeautifyBgStartB_, iconBeautifyBgOpacity_);
    const D2D1_COLOR_F border = luminance > 0.58f
        ? D2D1::ColorF(0.62f, 0.66f, 0.72f, iconBeautifyBgOpacity_)
        : D2D1::ColorF(0.78f, 0.82f, 0.90f, iconBeautifyBgOpacity_);
    DrawBeautifiedIconPlate(ctx, rect, fill, border, 1.0f);
    ComPtr<IDWriteTextFormat> format;
    format.Attach(CreateFaTextFormat(dwriteFactory_.Get(),
        static_cast<float>(std::min(rect.right - rect.left, rect.bottom - rect.top)) * 0.52f));
    if (format)
        DrawD2DText(ctx, L"", rect, format.Get(),
            D2D1::ColorF(1.0f, 219.0f / 255.0f, 118.0f / 255.0f, 0.94f));
}

void DesktopApp::DrawPlaceholderIcon(ID2D1RenderTarget* ctx, int sysIconIndex,
    RECT iconRect, float alpha, bool allowBeautify)
{
    if (!ctx || sysIconIndex < 0) return;

    // 快捷导航改走 DComp 后，ctx 必为 ID2D1DeviceContext（与桌面同源 d2dDevice_）。
    // 非 device-context 路径已废弃，直接返回以避免在错误设备上创建位图。
    ComPtr<ID2D1DeviceContext> deviceContext;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&deviceContext))) || !deviceContext || !d2dContext_)
        return;
    auto& cache = placeholderIconCache_;
    const bool beautify = allowBeautify && iconBeautifyEnabled_;
    const std::uint64_t cacheKey =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sysIconIndex)) << 1) |
        static_cast<std::uint64_t>(beautify ? 1 : 0);

    auto cached = cache.find(cacheKey);
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

        ComPtr<ID2D1Bitmap1> iconBitmap = CreateD2DBitmapFromHBitmap(alphaBitmap, beautify);
        DeleteObject(alphaBitmap);
        if (!iconBitmap)
        {
            return;
        }

        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(iconBitmap.As(&bitmap)) || !bitmap)
            return;

        cached = cache.emplace(cacheKey, std::move(bitmap)).first;
    }

    D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
        static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
    ctx->DrawBitmap(cached->second.Get(), dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void DesktopApp::DrawQuickNavSysIcon(ID2D1RenderTarget* ctx, int sysIconIndex, RECT dstRect)
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

        ComPtr<ID2D1Bitmap1> iconBitmap = CreateD2DBitmapFromHBitmap(
            alphaBitmap, iconBeautifyEnabled_);
        DeleteObject(alphaBitmap);
        if (!iconBitmap)
        {
            return;
        }

        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(iconBitmap.As(&bitmap)) || !bitmap)
            return;

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
