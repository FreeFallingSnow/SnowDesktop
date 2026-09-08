#include "../icon_bitmap_pixels.h"
#include "app.h"

// HBITMAP analysis, icon beautification and Direct2D bitmap caching.

namespace
{
    struct IconPixelBuffer
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> pixels;
    };

    struct IconVisibleBounds
    {
        bool hasVisiblePixels = false;
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    struct IconBackgroundColor
    {
        int r = 246;
        int g = 247;
        int b = 250;
    };

    std::uint8_t PixelA(std::uint32_t pixel)
    {
        return static_cast<std::uint8_t>((pixel >> 24) & 0xff);
    }

    std::uint32_t PackBgra(int b, int g, int r, int a)
    {
        return (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24) |
            (static_cast<std::uint32_t>(std::clamp(r, 0, 255)) << 16) |
            (static_cast<std::uint32_t>(std::clamp(g, 0, 255)) << 8) |
            static_cast<std::uint32_t>(std::clamp(b, 0, 255));
    }

    std::uint32_t PackPremultipliedRgb(int r, int g, int b, int a)
    {
        return PackBgra(
            (b * a + 127) / 255,
            (g * a + 127) / 255,
            (r * a + 127) / 255,
            a);
    }

    bool ReadHBitmapPixels(HBITMAP hbm, IconPixelBuffer& out)
    {
        BITMAP bm{};
        if (!hbm || GetObjectW(hbm, sizeof(bm), &bm) == 0)
            return false;

        const int width = bm.bmWidth;
        const int height = std::abs(bm.bmHeight);
        if (width <= 0 || height <= 0)
            return false;

        out.width = width;
        out.height = height;
        out.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

        if (bm.bmBits != nullptr && bm.bmBitsPixel == 32)
        {
            const auto* src = static_cast<const std::uint8_t*>(bm.bmBits);
            const int stride = std::abs(bm.bmWidthBytes);
            for (int y = 0; y < height; ++y)
            {
                std::memcpy(out.pixels.data() + static_cast<size_t>(y) * width,
                    src + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(width) * sizeof(std::uint32_t));
            }
            snowdesktop::icon_bitmap_pixels::NormalizeShellPixels(out.pixels);
            return true;
        }

        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
            return false;

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        const bool ok = GetDIBits(screenDc, hbm, 0, static_cast<UINT>(height),
            out.pixels.data(), &bitmapInfo, DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, screenDc);
        if (!ok)
            return false;

        snowdesktop::icon_bitmap_pixels::NormalizeShellPixels(out.pixels);
        return true;
    }

    IconVisibleBounds AnalyzeIconVisibleBounds(const std::vector<std::uint32_t>& pixels,
        int width, int height)
    {
        IconVisibleBounds bounds{};
        bounds.left = width;
        bounds.top = height;
        bounds.right = -1;
        bounds.bottom = -1;

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::uint8_t a = PixelA(pixels[static_cast<size_t>(y) * width + x]);
                if (a > 16)
                {
                    bounds.hasVisiblePixels = true;
                    bounds.left = std::min(bounds.left, x);
                    bounds.top = std::min(bounds.top, y);
                    bounds.right = std::max(bounds.right, x);
                    bounds.bottom = std::max(bounds.bottom, y);
                }
            }
        }

        return bounds;
    }

    bool DetectSolidEdgeBackground(const std::vector<std::uint32_t>& pixels,
        int width, int height, IconBackgroundColor& color)
    {
        const auto detected = snowdesktop::icon_beautify::DetectPlateFill(pixels, width, height);
        if (!detected) return false;
        color = {detected->r, detected->g, detected->b};
        return true;
    }

    int RoundedRectMaskAlpha(int x, int y, int width, int height, float radius)
    {
        const float px = static_cast<float>(x) + 0.5f;
        const float py = static_cast<float>(y) + 0.5f;
        const float left = radius;
        const float top = radius;
        const float right = static_cast<float>(width) - radius;
        const float bottom = static_cast<float>(height) - radius;

        float dx = 0.0f;
        if (px < left) dx = left - px;
        else if (px > right) dx = px - right;

        float dy = 0.0f;
        if (py < top) dy = top - py;
        else if (py > bottom) dy = py - bottom;

        // Superellipse corner: the larger radius offsets the softer continuous curve.
        const float distance = std::pow(
            std::pow(dx, kIconBeautifyCornerExponent) +
                std::pow(dy, kIconBeautifyCornerExponent),
            1.0f / kIconBeautifyCornerExponent);
        const float coverage = std::clamp(radius + 0.5f - distance, 0.0f, 1.0f);
        return static_cast<int>(std::round(coverage * 255.0f));
    }

    std::uint32_t ScalePremultipliedPixel(std::uint32_t pixel, int scale)
    {
        if (scale <= 0 || PixelA(pixel) == 0)
            return 0;
        if (scale >= 255)
            return pixel;

        const int b = static_cast<int>(pixel & 0xff);
        const int g = static_cast<int>((pixel >> 8) & 0xff);
        const int r = static_cast<int>((pixel >> 16) & 0xff);
        const int a = static_cast<int>((pixel >> 24) & 0xff);
        return PackBgra(
            (b * scale + 127) / 255,
            (g * scale + 127) / 255,
            (r * scale + 127) / 255,
            (a * scale + 127) / 255);
    }

    std::uint32_t SourceOverPremultiplied(std::uint32_t src, std::uint32_t dst)
    {
        const int sa = PixelA(src);
        if (sa == 0) return dst;
        if (sa == 255) return src;

        const int inv = 255 - sa;
        const int sb = static_cast<int>(src & 0xff);
        const int sg = static_cast<int>((src >> 8) & 0xff);
        const int sr = static_cast<int>((src >> 16) & 0xff);
        const int db = static_cast<int>(dst & 0xff);
        const int dg = static_cast<int>((dst >> 8) & 0xff);
        const int dr = static_cast<int>((dst >> 16) & 0xff);
        const int da = PixelA(dst);

        return PackBgra(
            sb + (db * inv + 127) / 255,
            sg + (dg * inv + 127) / 255,
            sr + (dr * inv + 127) / 255,
            sa + (da * inv + 127) / 255);
    }

    std::uint32_t SampleBgraBilinear(const std::vector<std::uint32_t>& pixels,
        int width, int height, float x, float y)
    {
        x = std::clamp(x, 0.0f, static_cast<float>(std::max(0, width - 1)));
        y = std::clamp(y, 0.0f, static_cast<float>(std::max(0, height - 1)));

        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(width - 1, x0 + 1);
        const int y1 = std::min(height - 1, y0 + 1);
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);

        const std::uint32_t p00 = pixels[static_cast<size_t>(y0) * width + x0];
        const std::uint32_t p10 = pixels[static_cast<size_t>(y0) * width + x1];
        const std::uint32_t p01 = pixels[static_cast<size_t>(y1) * width + x0];
        const std::uint32_t p11 = pixels[static_cast<size_t>(y1) * width + x1];

        auto channel = [&](int shift) {
            const float c00 = static_cast<float>((p00 >> shift) & 0xff);
            const float c10 = static_cast<float>((p10 >> shift) & 0xff);
            const float c01 = static_cast<float>((p01 >> shift) & 0xff);
            const float c11 = static_cast<float>((p11 >> shift) & 0xff);
            const float top = c00 + (c10 - c00) * fx;
            const float bottom = c01 + (c11 - c01) * fx;
            return static_cast<int>(std::round(top + (bottom - top) * fy));
        };

        return PackBgra(channel(0), channel(8), channel(16), channel(24));
    }

    struct IconBackgroundPaint
    {
        IconBackgroundColor start{};
        IconBackgroundColor end{};
        IconBackgroundColor border{};
        int opacity = 255;
        bool gradient = false;
        int gradientDirection = 0;
    };

    int IconColorLuma(const IconBackgroundColor& color)
    {
        return (color.r * 299 + color.g * 587 + color.b * 114) / 1000;
    }

    IconBackgroundColor MixIconColor(
        const IconBackgroundColor& start,
        const IconBackgroundColor& end,
        float amount)
    {
        amount = std::clamp(amount, 0.0f, 1.0f);
        return IconBackgroundColor{
            std::clamp(static_cast<int>(std::round(
                static_cast<float>(start.r) + static_cast<float>(end.r - start.r) * amount)), 0, 255),
            std::clamp(static_cast<int>(std::round(
                static_cast<float>(start.g) + static_cast<float>(end.g - start.g) * amount)), 0, 255),
            std::clamp(static_cast<int>(std::round(
                static_cast<float>(start.b) + static_cast<float>(end.b - start.b) * amount)), 0, 255)
        };
    }

    IconBackgroundColor AutoIconBorderColor(
        const IconBackgroundColor& start,
        const IconBackgroundColor& end)
    {
        const IconBackgroundColor mid = MixIconColor(start, end, 0.5f);
        const int delta = IconColorLuma(mid) >= 128 ? -34 : 34;
        return IconBackgroundColor{
            std::clamp(mid.r + delta, 0, 255),
            std::clamp(mid.g + delta, 0, 255),
            std::clamp(mid.b + delta, 0, 255)
        };
    }

    IconBackgroundColor IconColorFromFloats(float r, float g, float b)
    {
        return IconBackgroundColor{
            std::clamp(static_cast<int>(std::round(std::clamp(r, 0.0f, 1.0f) * 255.0f)), 0, 255),
            std::clamp(static_cast<int>(std::round(std::clamp(g, 0.0f, 1.0f) * 255.0f)), 0, 255),
            std::clamp(static_cast<int>(std::round(std::clamp(b, 0.0f, 1.0f) * 255.0f)), 0, 255)
        };
    }

    void FillRoundedIconBackground(std::vector<std::uint32_t>& output,
        int width,
        int height,
        float radius,
        const IconBackgroundPaint& paint)
    {
        for (int y = 0; y < height; ++y)
        {
            const float yT = height > 1
                ? static_cast<float>(y) / static_cast<float>(height - 1)
                : 0.0f;
            for (int x = 0; x < width; ++x)
            {
                const float xT = width > 1
                    ? static_cast<float>(x) / static_cast<float>(width - 1)
                    : 0.0f;
                float gradientT = 0.0f;
                if (paint.gradient)
                {
                    switch (paint.gradientDirection)
                    {
                    case 1: gradientT = xT; break;
                    case 2: gradientT = (xT + yT) * 0.5f; break;
                    case 3: gradientT = (xT + (1.0f - yT)) * 0.5f; break;
                    default: gradientT = yT; break;
                    }
                }
                const IconBackgroundColor fill = MixIconColor(paint.start, paint.end, gradientT);
                const int mask = RoundedRectMaskAlpha(x, y, width, height, radius);
                const int innerWidth = std::max(1, width - 2);
                const int innerHeight = std::max(1, height - 2);
                const int innerMask = RoundedRectMaskAlpha(
                    x - 1, y - 1, innerWidth, innerHeight, std::max(1.0f, radius - 1.0f));
                const float borderMix = mask > 0
                    ? static_cast<float>(std::clamp(mask - innerMask, 0, 255)) / static_cast<float>(mask)
                    : 0.0f;
                const int r = static_cast<int>(std::round(
                    static_cast<float>(fill.r) + static_cast<float>(paint.border.r - fill.r) * borderMix));
                const int g = static_cast<int>(std::round(
                    static_cast<float>(fill.g) + static_cast<float>(paint.border.g - fill.g) * borderMix));
                const int b = static_cast<int>(std::round(
                    static_cast<float>(fill.b) + static_cast<float>(paint.border.b - fill.b) * borderMix));
                const int alpha = (mask * paint.opacity + 127) / 255;
                output[static_cast<size_t>(y) * width + x] = PackPremultipliedRgb(r, g, b, alpha);
            }
        }
    }

    void ApplyRoundedIconOutline(std::vector<std::uint32_t>& output,
        int width,
        int height,
        float radius,
        IconBackgroundColor stroke,
        int opacity)
    {
        const int innerWidth = std::max(1, width - 2);
        const int innerHeight = std::max(1, height - 2);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int mask = RoundedRectMaskAlpha(x, y, width, height, radius);
                if (mask <= 0)
                    continue;

                const int innerMask = RoundedRectMaskAlpha(
                    x - 1, y - 1, innerWidth, innerHeight, std::max(1.0f, radius - 1.0f));
                const int edgeAlpha = std::clamp(mask - innerMask, 0, 255);
                if (edgeAlpha <= 0)
                    continue;

                const int alpha = (edgeAlpha * opacity + 127) / 255;
                std::uint32_t& dst = output[static_cast<size_t>(y) * width + x];
                dst = SourceOverPremultiplied(
                    PackPremultipliedRgb(stroke.r, stroke.g, stroke.b, alpha),
                    dst);
            }
        }
    }

    struct IconShadowPass
    {
        int dx = 0;
        int dy = 0;
        int opacity = 0;
    };

    std::uint32_t MakeIconSourceShadow(std::uint32_t sampled, int mask, int opacity)
    {
        if (mask <= 0 || opacity <= 0)
            return 0;

        const int alpha = PixelA(sampled);
        if (alpha <= 0)
            return 0;

        const int shadowAlpha = (alpha * mask * opacity + 255 * 255 / 2) / (255 * 255);
        return PackPremultipliedRgb(48, 58, 72, shadowAlpha);
    }

    std::vector<std::uint32_t> BeautifyIconPixels(
        const std::vector<std::uint32_t>& source,
        int width,
        int height,
        const IconBackgroundPaint& backgroundPaint,
        float cornerRadius,
        bool smartRecognitionEnabled)
    {
        const IconVisibleBounds bounds = AnalyzeIconVisibleBounds(source, width, height);
        if (!bounds.hasVisiblePixels)
            return source;

        const float radius = cornerRadius;
        IconBackgroundColor edgeFill{};
        const bool clipWithEdgeFill = smartRecognitionEnabled &&
            DetectSolidEdgeBackground(source, width, height, edgeFill);
        std::vector<std::uint32_t> output(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

        if (clipWithEdgeFill)
        {
            const std::uint32_t background = PackPremultipliedRgb(edgeFill.r, edgeFill.g, edgeFill.b, 255);
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const int mask = RoundedRectMaskAlpha(x, y, width, height, radius);
                    const std::uint32_t composed = SourceOverPremultiplied(
                        source[static_cast<size_t>(y) * width + x], background);
                    output[static_cast<size_t>(y) * width + x] =
                        ScalePremultipliedPixel(composed, mask);
                }
            }
            if (IconColorLuma(edgeFill) >= 232)
            {
                ApplyRoundedIconOutline(output, width, height, radius,
                    IconBackgroundColor{ 190, 199, 214 }, 150);
            }
            return output;
        }

        FillRoundedIconBackground(output, width, height, radius, backgroundPaint);

        const int sourceW = std::max(1, bounds.right - bounds.left + 1);
        const int sourceH = std::max(1, bounds.bottom - bounds.top + 1);
        const int padding = std::max(5, static_cast<int>(std::round(std::min(width, height) * 0.16f)));
        const int maxW = std::max(1, width - padding * 2);
        const int maxH = std::max(1, height - padding * 2);
        const float scale = std::min(
            static_cast<float>(maxW) / static_cast<float>(sourceW),
            static_cast<float>(maxH) / static_cast<float>(sourceH));
        const int destW = std::max(1, static_cast<int>(std::round(sourceW * scale)));
        const int destH = std::max(1, static_cast<int>(std::round(sourceH * scale)));
        const int destLeft = (width - destW) / 2;
        const int destTop = (height - destH) / 2;

        constexpr IconShadowPass kShadowPasses[] = {
            { 0, 1, 42 },
            { -1, 1, 22 },
            { 1, 1, 22 },
            { 0, 2, 18 },
            { -1, 0, 14 },
            { 1, 0, 14 },
            { 0, -1, 10 },
        };

        for (int y = 0; y < destH; ++y)
        {
            for (int x = 0; x < destW; ++x)
            {
                const float sx = static_cast<float>(bounds.left) +
                    ((static_cast<float>(x) + 0.5f) / static_cast<float>(destW)) *
                    static_cast<float>(sourceW) - 0.5f;
                const float sy = static_cast<float>(bounds.top) +
                    ((static_cast<float>(y) + 0.5f) / static_cast<float>(destH)) *
                    static_cast<float>(sourceH) - 0.5f;
                const std::uint32_t sampled = SampleBgraBilinear(source, width, height, sx, sy);
                if (PixelA(sampled) == 0)
                    continue;

                const int outX = destLeft + x;
                const int outY = destTop + y;
                for (const IconShadowPass& pass : kShadowPasses)
                {
                    const int shadowX = outX + pass.dx;
                    const int shadowY = outY + pass.dy;
                    if (shadowX < 0 || shadowY < 0 || shadowX >= width || shadowY >= height)
                        continue;

                    const int mask = RoundedRectMaskAlpha(shadowX, shadowY, width, height, radius);
                    std::uint32_t shadow = MakeIconSourceShadow(sampled, mask, pass.opacity);
                    std::uint32_t& dst = output[static_cast<size_t>(shadowY) * width + shadowX];
                    dst = SourceOverPremultiplied(shadow, dst);
                }
            }
        }

        for (int y = 0; y < destH; ++y)
        {
            for (int x = 0; x < destW; ++x)
            {
                const float sx = static_cast<float>(bounds.left) +
                    ((static_cast<float>(x) + 0.5f) / static_cast<float>(destW)) *
                    static_cast<float>(sourceW) - 0.5f;
                const float sy = static_cast<float>(bounds.top) +
                    ((static_cast<float>(y) + 0.5f) / static_cast<float>(destH)) *
                    static_cast<float>(sourceH) - 0.5f;
                std::uint32_t sampled = SampleBgraBilinear(source, width, height, sx, sy);

                const int outX = destLeft + x;
                const int outY = destTop + y;
                if (outX < 0 || outY < 0 || outX >= width || outY >= height)
                    continue;

                const int mask = RoundedRectMaskAlpha(outX, outY, width, height, radius);
                sampled = ScalePremultipliedPixel(sampled, mask);
                std::uint32_t& dst = output[static_cast<size_t>(outY) * width + outX];
                dst = SourceOverPremultiplied(sampled, dst);
            }
        }

        return output;
    }
}

std::uintptr_t DesktopApp::GetD2DIconCacheKey(HBITMAP hbm, bool beautified) const
{
    std::uintptr_t key = reinterpret_cast<std::uintptr_t>(hbm);
    if (!beautified)
        return key;

    if constexpr (sizeof(std::uintptr_t) >= 8)
        return key ^ static_cast<std::uintptr_t>(0x9e3779b97f4a7c15ull);
    else
        return key ^ static_cast<std::uintptr_t>(0x9e3779b9u);
}

void DesktopApp::EraseD2DIconCacheForBitmap(HBITMAP hbm)
{
    if (!hbm) return;
    d2dIconCache_.erase(GetD2DIconCacheKey(hbm, false));
    d2dIconCache_.erase(GetD2DIconCacheKey(hbm, true));
}

ComPtr<ID2D1Bitmap1> DesktopApp::CreateD2DBitmapFromHBitmap(
    HBITMAP hbm, bool beautify)
{
    if (!hbm || !d2dContext_)
        return nullptr;

    IconPixelBuffer buffer;
    if (!ReadHBitmapPixels(hbm, buffer))
        return nullptr;

    if (beautify)
    {
        std::optional<snowdesktop::icon_beautify::EdgeColor> edgeFill;
        if (iconBeautifySettings_.mode == 0)
        {
            IconBackgroundColor detected{};
            if (DetectSolidEdgeBackground(
                    buffer.pixels, buffer.width, buffer.height, detected))
            {
                edgeFill = snowdesktop::icon_beautify::EdgeColor{
                    detected.r, detected.g, detected.b };
            }
        }
        buffer.pixels = snowdesktop::icon_beautify::Render(
            buffer.pixels, buffer.width, buffer.height,
            iconBeautifySettings_, edgeFill);
    }

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(d2dContext_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(buffer.width), static_cast<UINT32>(buffer.height)),
            buffer.pixels.data(),
            static_cast<UINT32>(buffer.width * sizeof(std::uint32_t)),
            &props,
            &bitmap)))
    {
        return nullptr;
    }

    return bitmap;
}

/**
 * @brief 获取或创建 HBITMAP 对应的 Direct2D 位图（带缓存）。
 * @param hbm HBITMAP 句柄。
 * @return ID2D1Bitmap1 指针，失败返回 nullptr。
 */
ID2D1Bitmap1* DesktopApp::GetOrCreateD2DBitmap(HBITMAP hbm)
{
    return GetOrCreateD2DBitmap(hbm, iconBeautifySettings_.enabled);
}

ID2D1Bitmap1* DesktopApp::GetOrCreateD2DBitmap(HBITMAP hbm, bool beautify)
{
    if (!hbm) return nullptr;
    const auto key = GetD2DIconCacheKey(hbm, beautify);
    auto it = d2dIconCache_.find(key);
    if (it != d2dIconCache_.end()) return it->second.Get();

    ComPtr<ID2D1Bitmap1> bitmap = CreateD2DBitmapFromHBitmap(hbm, beautify);
    if (!bitmap)
        return nullptr;

    auto* result = bitmap.Get();
    d2dIconCache_[key] = std::move(bitmap);
    return result;
}

ID2D1Bitmap* DesktopApp::GetOrCreateD2DBitmap(
    ID2D1RenderTarget* target, HBITMAP hbm, bool beautify)
{
    if (!target || !hbm) return nullptr;

    // 快捷导航改走 DComp 后，target 必为 ID2D1DeviceContext（与桌面同源 d2dDevice_），
    // 统一走 d2dIconCache_；非 device-context 路径已废弃。
    ComPtr<ID2D1DeviceContext> deviceContext;
    if (FAILED(target->QueryInterface(IID_PPV_ARGS(&deviceContext))) || !deviceContext)
        return nullptr;
    return GetOrCreateD2DBitmap(hbm, beautify);
}

void DesktopApp::DrawIconBitmap(ID2D1RenderTarget* target,
    ID2D1Bitmap* bitmap, RECT destination, float opacity)
{
    if (!target || !bitmap || IsRectEmptyRect(destination))
        return;

    const D2D1_SIZE_U source = bitmap->GetPixelSize();
    const auto fitted = snowdesktop::icon_render_rules::FitWithoutUpscaling(
        static_cast<int>(source.width), static_cast<int>(source.height),
        destination.right - destination.left,
        destination.bottom - destination.top);
    if (fitted.width <= 0 || fitted.height <= 0)
        return;

    const int left = destination.left +
        (destination.right - destination.left - fitted.width) / 2;
    const int top = destination.top +
        (destination.bottom - destination.top - fitted.height) / 2;
    const D2D1_RECT_F dst = D2D1::RectF(
        static_cast<float>(left), static_cast<float>(top),
        static_cast<float>(left + fitted.width),
        static_cast<float>(top + fitted.height));

    ComPtr<ID2D1DeviceContext> deviceContext;
    if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&deviceContext))) &&
        deviceContext)
    {
        deviceContext->DrawBitmap(bitmap, &dst, opacity,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
            nullptr, nullptr);
        return;
    }
    target->DrawBitmap(bitmap, dst, opacity,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}
