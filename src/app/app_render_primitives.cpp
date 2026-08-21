#include "app.h"

// Reusable Direct2D drawing primitives.

void DesktopApp::DrawD2DRoundedRectangle(ID2D1RenderTarget* ctx, RECT rect, float radius,
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

void DesktopApp::DrawWidgetPanelBackground(ID2D1DeviceContext* ctx, RECT frame, float radius,
    D2D1_COLOR_F fill, D2D1_COLOR_F border, bool selected, float strokeWidth,
    const PersonalizationSettings* effectSettings, bool registerBackdrop)
{
    if (!ctx || IsRectEmptyRect(frame)) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }

    PersonalizationSettings p = effectSettings
        ? *effectSettings
        : (settingsWindow_
            ? settingsWindow_->GetPersonalization()
            : PersonalizationSettings::DarkPreset());
    radius = std::max(0.0f, radius);

    auto getBrush = [&](const D2D1_COLOR_F& c) -> ID2D1SolidColorBrush* {
        const auto key = D2DColorBrushKey(c);
        auto it = brushCache_.find(key);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> b;
            if (FAILED(ctx->CreateSolidColorBrush(c, &b)) || !b) return nullptr;
            it = brushCache_.emplace(key, std::move(b)).first;
        }
        return it->second.Get();
    };

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(ToD2DRect(frame), radius, radius);

    // 原生毛玻璃由下层 CompositionBackdropBrush 提供，本层只绘制色调和装饰。
    if (p.glassEnabled && registerBackdrop)
    {
        bool registered = false;
        if (renderingFloatingDock_)
        {
            registered = floatingDockBackdropCompositor_.AddPanel(
                snowdesktop::floating_dock_rules::
                    DesktopRectToWindowRect(
                        frame, floatingDockSourceRect_),
                radius, p.glassBlurRadius);
        }
        else
        {
            registered = desktopBackdropCompositor_.AddPanel(
                frame, radius, p.glassBlurRadius);
        }
        if (realtimeCompositionDrawInProgress_ && registered)
            realtimeBackdropRegisteredDuringDraw_ = true;
    }

    if (fill.a > 0.0f)
    {
        if (auto* fillBrush = getBrush(fill))
            ctx->FillRoundedRectangle(rr, fillBrush);
    }
    if (p.glassEnabled && p.acrylicEnabled)
    {
        POINT screenOrigin{};
        // Both render paths use desktop-client coordinates. Anchor the acrylic
        // texture to that common coordinate space so the Dock does not appear
        // to change border/noise treatment when moved to the floating host.
        HWND renderWindow = hwnd_;
        if (renderWindow)
            ClientToScreen(renderWindow, &screenOrigin);
        DrawAcrylicNoise(ctx, frame, radius, p.contentTheme == 1,
            screenOrigin);
    }

    D2D1_COLOR_F stroke = selected
        ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.90f)
        : border;
    if (stroke.a > 0.0f)
    {
        const bool glassDrawn = p.glassEnabled && !selected &&
            DrawGlassBorder(ctx, frame, radius, stroke, strokeWidth);
        if (!glassDrawn)
        {
            if (auto* strokeBrush = getBrush(stroke))
                ctx->DrawRoundedRectangle(rr, strokeBrush, strokeWidth, nullptr);
        }
    }
}

void DesktopApp::DrawAcrylicNoise(ID2D1DeviceContext* ctx, RECT frame,
    float radius, bool lightTheme, POINT screenOrigin)
{
    if (!ctx || IsRectEmptyRect(frame))
        return;

    constexpr UINT kNoiseSize = 64;
    const std::uintptr_t contextKey =
        reinterpret_cast<std::uintptr_t>(ctx) & ~std::uintptr_t{1};
    const std::uintptr_t cacheKey = contextKey |
        static_cast<std::uintptr_t>(lightTheme);
    auto found = acrylicNoiseBrushCache_.find(cacheKey);
    if (found == acrylicNoiseBrushCache_.end())
    {
        if (acrylicNoiseBrushCache_.size() >= 8)
            acrylicNoiseBrushCache_.clear();

        std::array<std::uint32_t, kNoiseSize * kNoiseSize> pixels{};
        std::uint32_t state = 0x534E4F57u; // "SNOW", fixed seed.
        for (std::uint32_t& pixel : pixels)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            // System acrylic uses a very subtle texture. Keep alpha between
            // roughly 0.8% and 3.1%, with polarity selected by content theme.
            const std::uint8_t alpha = static_cast<std::uint8_t>(
                2u + ((state >> 24) & 0x06u));
            const std::uint8_t channel = lightTheme ? 0u : alpha;
            pixel = (static_cast<std::uint32_t>(alpha) << 24) |
                (static_cast<std::uint32_t>(channel) << 16) |
                (static_cast<std::uint32_t>(channel) << 8) |
                static_cast<std::uint32_t>(channel);
        }

        D2D1_BITMAP_PROPERTIES1 bitmapProperties =
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> bitmap;
        const D2D1_SIZE_U bitmapSize =
            D2D1::SizeU(kNoiseSize, kNoiseSize);
        if (FAILED(ctx->CreateBitmap(bitmapSize, pixels.data(),
                kNoiseSize * sizeof(std::uint32_t), &bitmapProperties,
                &bitmap)) || !bitmap)
            return;

        D2D1_BITMAP_BRUSH_PROPERTIES1 brushProperties =
            D2D1::BitmapBrushProperties1(
                D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        ComPtr<ID2D1BitmapBrush1> brush;
        if (FAILED(ctx->CreateBitmapBrush(bitmap.Get(), &brushProperties,
                nullptr, &brush)) || !brush)
            return;
        found = acrylicNoiseBrushCache_.emplace(cacheKey,
            std::move(brush)).first;
    }

    if (found->second)
    {
        // Keep the tile aligned to physical screen pixels. Redrawing or moving
        // a panel therefore samples the same noise instead of making the
        // texture appear to shimmer.
        found->second->SetTransform(D2D1::Matrix3x2F::Translation(
            -static_cast<float>(screenOrigin.x),
            -static_cast<float>(screenOrigin.y)));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(
            ToD2DRect(frame), radius, radius), found->second.Get());
    }
}

void DesktopApp::DrawD2DFilledRectangle(ID2D1RenderTarget* ctx, RECT rect,
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
