#include "app.h"

// Native glass border rendering and diagnostics.

bool DesktopApp::DrawGlassBorder(ID2D1DeviceContext* ctx, RECT frame,
    float radius, D2D1_COLOR_F color, float strokeWidth)
{
    if (!ctx || color.a <= 0.0f || IsRectEmptyRect(frame))
        return false;

    auto mixWhite = [](float value, float amount) {
        return std::clamp(value + (1.0f - value) * amount, 0.0f, 1.0f);
    };
    const D2D1_COLOR_F bright = D2D1::ColorF(
        mixWhite(color.r, 0.58f), mixWhite(color.g, 0.58f),
        mixWhite(color.b, 0.58f),
        std::clamp(color.a * 0.91f, 0.0f, 1.0f));
    const D2D1_COLOR_F lowerRight = D2D1::ColorF(
        mixWhite(color.r, 0.18f), mixWhite(color.g, 0.18f),
        mixWhite(color.b, 0.18f),
        std::clamp(color.a * 0.82f, 0.0f, 1.0f));
    const D2D1_GRADIENT_STOP upperLeftStops[] = {
        { 0.0f, bright },
        { 0.46f, D2D1::ColorF(bright.r, bright.g, bright.b,
            bright.a * 0.55f) },
        { 0.82f, D2D1::ColorF(bright.r, bright.g, bright.b,
            bright.a * 0.20f) },
        { 1.0f, D2D1::ColorF(
            bright.r, bright.g, bright.b, bright.a * 0.12f) },
    };
    const D2D1_GRADIENT_STOP lowerRightStops[] = {
        { 0.0f, lowerRight },
        { 0.46f, D2D1::ColorF(lowerRight.r, lowerRight.g, lowerRight.b,
            lowerRight.a * 0.55f) },
        { 0.82f, D2D1::ColorF(lowerRight.r, lowerRight.g, lowerRight.b,
            lowerRight.a * 0.20f) },
        { 1.0f, D2D1::ColorF(
            lowerRight.r, lowerRight.g, lowerRight.b, lowerRight.a * 0.12f) },
    };
    ComPtr<ID2D1GradientStopCollection> upperLeftCollection;
    ComPtr<ID2D1GradientStopCollection> lowerRightCollection;
    if (FAILED(ctx->CreateGradientStopCollection(upperLeftStops,
            static_cast<UINT32>(std::size(upperLeftStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &upperLeftCollection)) ||
        !upperLeftCollection ||
        FAILED(ctx->CreateGradientStopCollection(lowerRightStops,
            static_cast<UINT32>(std::size(lowerRightStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &lowerRightCollection)) ||
        !lowerRightCollection)
        return false;

    auto createCornerBrush = [ctx](const D2D1_RECT_F& bounds,
        bool useLowerRight, ID2D1GradientStopCollection* stops,
        ComPtr<ID2D1RadialGradientBrush>& brush) {
        const float width = bounds.right - bounds.left;
        const float height = bounds.bottom - bounds.top;
        const D2D1_POINT_2F center = useLowerRight
            ? D2D1::Point2F(bounds.right, bounds.bottom)
            : D2D1::Point2F(bounds.left, bounds.top);
        return SUCCEEDED(ctx->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(center,
                D2D1::Point2F(0.0f, 0.0f), width, height),
            stops, &brush)) && brush;
    };

    const D2D1_RECT_F outerRect = ToD2DRect(frame);
    ComPtr<ID2D1RadialGradientBrush> upperLeftBrush;
    ComPtr<ID2D1RadialGradientBrush> lowerRightBrush;
    if (!createCornerBrush(outerRect, false, upperLeftCollection.Get(),
            upperLeftBrush) ||
        !createCornerBrush(outerRect, true, lowerRightCollection.Get(),
            lowerRightBrush))
        return false;

    const D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(
        outerRect, radius, radius);
    for (ID2D1RadialGradientBrush* brush :
        { upperLeftBrush.Get(), lowerRightBrush.Get() })
    {
        brush->SetOpacity(0.24f);
        ctx->DrawRoundedRectangle(outer, brush, strokeWidth + 1.35f);
        brush->SetOpacity(1.0f);
        ctx->DrawRoundedRectangle(outer, brush, strokeWidth);
    }

    const float inset = std::max(0.85f, strokeWidth * 0.85f);
    const D2D1_RECT_F innerRect = D2D1::RectF(
        frame.left + inset, frame.top + inset,
        frame.right - inset, frame.bottom - inset);
    if (innerRect.right > innerRect.left && innerRect.bottom > innerRect.top)
    {
        const float darkAlpha = std::clamp(color.a * 0.30f, 0.015f, 0.14f);
        const D2D1_GRADIENT_STOP innerStops[] = {
            { 0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, darkAlpha) },
            { 0.46f, D2D1::ColorF(
                0.0f, 0.0f, 0.0f, darkAlpha * 0.32f) },
            { 0.82f, D2D1::ColorF(
                0.0f, 0.0f, 0.0f, darkAlpha * 0.10f) },
            { 1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) },
        };
        ComPtr<ID2D1GradientStopCollection> innerCollection;
        ComPtr<ID2D1RadialGradientBrush> innerUpperLeftBrush;
        ComPtr<ID2D1RadialGradientBrush> innerLowerRightBrush;
        if (SUCCEEDED(ctx->CreateGradientStopCollection(innerStops,
                static_cast<UINT32>(std::size(innerStops)), D2D1_GAMMA_2_2,
                D2D1_EXTEND_MODE_CLAMP, &innerCollection)) &&
            innerCollection &&
            createCornerBrush(innerRect, false, innerCollection.Get(),
                innerUpperLeftBrush) &&
            createCornerBrush(innerRect, true, innerCollection.Get(),
                innerLowerRightBrush))
        {
            const D2D1_ROUNDED_RECT inner = D2D1::RoundedRect(innerRect,
                std::max(0.0f, radius - inset),
                std::max(0.0f, radius - inset));
            const float innerStroke = std::max(
                0.65f, strokeWidth * 0.65f);
            ctx->DrawRoundedRectangle(
                inner, innerUpperLeftBrush.Get(), innerStroke);
            ctx->DrawRoundedRectangle(
                inner, innerLowerRightBrush.Get(), innerStroke);
        }
    }
    return true;
}

/** @brief 返回设置界面显示的原生毛玻璃合成状态。 */
std::wstring DesktopApp::GetGlassBackendStatusText() const
{
    if (desktopBackdropCompositor_.IsAvailable())
    {
        std::wstring status = _LW("glass.dwm_enabled");
        status += _LW("glass.glass_panel");
        status += std::to_wstring(desktopBackdropCompositor_.PanelCount());
        status += _LW("glass.syncing");
        return status;
    }

    std::wstring status = _LW("glass.dwm_unavailable");
    if (!desktopBackdropCompositor_.LastError().empty())
    {
        status += L"：";
        status += desktopBackdropCompositor_.LastError();
    }
    return status;
}
