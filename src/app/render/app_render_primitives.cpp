#include "app/app.h"
#include "common/utils.h"
#include "app/dock/persistent_dock_host.h"
#include "widget/preview/widget_preview_stage.h"
#include "theme/panel_gradient_renderer.h"

void DesktopApp::DrawD2DRoundedRectangle(ID2D1RenderTarget* ctx, RECT rect, float radius,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth)
{
    drawing_.DrawRoundedRectangle(ctx, rect, radius, fill, stroke, strokeWidth);
}

void DesktopApp::DrawD2DRoundedRectangle(ID2D1RenderTarget* ctx, D2D1_RECT_F rect,
    float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth)
{
    drawing_.DrawRoundedRectangle(ctx, rect, radius, fill, stroke, strokeWidth);
}

void DesktopApp::DrawWidgetPanelBackground(ID2D1DeviceContext* ctx, RECT frame, float radius,
    D2D1_COLOR_F fill, D2D1_COLOR_F border, bool selected, float strokeWidth,
    const PersonalizationSettings* effectSettings, bool registerBackdrop,
    std::uintptr_t backdropOwnerKey, float effectScale)
{
    if (!ctx || IsRectEmptyRect(frame)) return;
    PersonalizationSettings p = effectSettings
        ? *effectSettings
        : CurrentPersonalization();
    radius = std::max(0.0f, radius);

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(ToD2DRect(frame), radius, radius);

    // 原生毛玻璃由下层 CompositionBackdropBrush 提供，本层只绘制色调和装饰。
    if (p.glassEnabled && registerBackdrop && desktopWidgetCompositionDrawInProgress_)
    {
        desktopWidgetBackdropRequestedDuringDraw_ = true;
        desktopWidgetBackdropCornerRadiusDuringDraw_ = radius;
        desktopWidgetBackdropBlurRadiusDuringDraw_ = p.glassBlurRadius;
    }
    else if (p.glassEnabled && registerBackdrop)
    {
        if (renderingFloatingDock_)
        {
            if (!renderingPersistentDockHost_)
                return;
            renderingPersistentDockHost_->backdrop.AddPanel(
                snowdesktop::floating_dock_rules::
                    DesktopRectToWindowRect(
                        frame,
                        renderingPersistentDockHost_->sourceRect),
                radius, p.glassBlurRadius,
                backdropOwnerKey, true);
        }
        else
        {
            desktopBackdropCompositor_.AddPanel(
                frame, radius, p.glassBlurRadius,
                backdropOwnerKey, true);
        }
    }

    if (!snowdesktop::DrawPanelGradient(ctx, ToD2DRect(frame), radius, p.panelGradient, 1.f, true) && fill.a > 0.0f)
    {
        if (auto fillBrush = drawing_.GetSolidBrush(ctx, fill))
            snowdesktop::smooth_corners::Fill(ctx, rr.rect, radius, fillBrush.Get());
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

    DrawWidgetPanelBorder(ctx, frame, radius, border, selected, strokeWidth);
    if (!selected)
        (void)DrawWidgetPanelEdgeHighlight(
            ctx, frame, radius, fill, &p, effectScale);
}

void DesktopApp::DrawWidgetPanelBorder(
    ID2D1DeviceContext* ctx, RECT frame, float radius,
    D2D1_COLOR_F border, bool selected, float strokeWidth)
{
    if (!ctx || IsRectEmptyRect(frame)) return;
    const D2D1_COLOR_F stroke = selected
        ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.90f)
        : border;
    if (stroke.a <= 0.0f) return;

    radius = std::max(0.0f, radius);
    strokeWidth = std::max(kMinimumWidgetBorderWidth, strokeWidth);
    auto outline = ToD2DRect(frame);
    if (!selected)
    {
        const float inset = strokeWidth * 0.5f;
        outline = D2D1::RectF(outline.left + inset, outline.top + inset,
            outline.right - inset, outline.bottom - inset);
        radius = std::max(0.0f, radius - inset);
    }
    DrawD2DRoundedRectangle(ctx, outline, radius,
        D2D1::ColorF(0, 0.0f), stroke, strokeWidth);
}

bool DesktopApp::DrawWidgetPanelEdgeHighlight(
    ID2D1DeviceContext* ctx, RECT frame, float radius,
    D2D1_COLOR_F fill, const PersonalizationSettings* effectSettings,
    float effectScale)
{
    if (!ctx || IsRectEmptyRect(frame)) return false;
    const PersonalizationSettings p = effectSettings
        ? *effectSettings
        : CurrentPersonalization();
    if (!p.widgetEdgeHighlightEnabled ||
        p.widgetEdgeHighlightStrength <= 0.0005f)
        return false;

    // Edge light belongs to the panel material, not to the optional outline.
    // Keep it stable when the border is transparent or recolored.
    const float edgeWidth = std::clamp(p.widgetEdgeHighlightWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth) *
        std::max(0.0f, effectScale);
    return DrawEdgeHighlight(ctx, frame, std::max(0.0f, radius), fill,
        edgeWidth, p.widgetEdgeHighlightStrength,
        p.edgeLight);
}

void DesktopApp::DrawAcrylicNoise(ID2D1DeviceContext* ctx, RECT frame,
    float radius, bool lightTheme, POINT screenOrigin)
{
    if (!ctx || IsRectEmptyRect(frame))
        return;

    constexpr UINT kNoiseSize = static_cast<UINT>(
        snowdesktop::widget_preview::AcrylicNoiseSize);
    const std::uintptr_t contextKey =
        reinterpret_cast<std::uintptr_t>(ctx) & ~std::uintptr_t{1};
    const std::uintptr_t cacheKey = contextKey |
        static_cast<std::uintptr_t>(lightTheme);
    auto found = acrylicNoiseBrushCache_.find(cacheKey);
    if (found == acrylicNoiseBrushCache_.end())
    {
        if (acrylicNoiseBrushCache_.size() >= 8)
            acrylicNoiseBrushCache_.clear();

        const auto& pixels =
            snowdesktop::widget_preview::GenerateAcrylicNoise(lightTheme);
        if (pixels.empty()) return;

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
        brush->SetOpacity(snowdesktop::widget_preview::AcrylicNoiseOpacity);
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
        snowdesktop::smooth_corners::Fill(ctx, ToD2DRect(frame), radius, found->second.Get());
    }
}

void DesktopApp::DrawD2DFilledRectangle(ID2D1RenderTarget* ctx, RECT rect,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke)
{
    drawing_.DrawFilledRectangle(ctx, rect, fill, stroke);
}
