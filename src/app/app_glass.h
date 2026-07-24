/**
 * @file app_glass.h
 * @brief DesktopApp 原生毛玻璃边框与状态展示。
 *
 * 背景模糊由 DesktopBackdropCompositor 和 DWM 完成；本文件只保留
 * Direct2D 内容层中的玻璃边框，以及设置界面的原生合成状态文本。
 */
#pragma once

/**
 * @brief 绘制液态玻璃边缘。
 * @details 左上角使用柔亮斜向高光，右下角收暗，内侧暗边提供厚度感。
 */
inline bool DesktopApp::DrawGlassBorder(ID2D1DeviceContext* ctx, RECT frame,
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
    const D2D1_COLOR_F middle = D2D1::ColorF(color.r, color.g, color.b,
        std::clamp(color.a * 0.67f, 0.0f, 1.0f));
    const D2D1_COLOR_F dim = D2D1::ColorF(
        color.r * 0.72f, color.g * 0.72f, color.b * 0.72f,
        std::clamp(color.a * 0.28f, 0.0f, 1.0f));
    const D2D1_GRADIENT_STOP outerStops[] = {
        { 0.0f, bright },
        { 0.38f, middle },
        { 0.72f, D2D1::ColorF(color.r, color.g, color.b, color.a * 0.42f) },
        { 1.0f, dim },
    };
    ComPtr<ID2D1GradientStopCollection> outerCollection;
    ComPtr<ID2D1LinearGradientBrush> outerBrush;
    if (FAILED(ctx->CreateGradientStopCollection(outerStops,
            static_cast<UINT32>(std::size(outerStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &outerCollection)) || !outerCollection ||
        FAILED(ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(static_cast<float>(frame.left),
                    static_cast<float>(frame.top)),
                D2D1::Point2F(static_cast<float>(frame.right),
                    static_cast<float>(frame.bottom))),
            outerCollection.Get(), &outerBrush)) || !outerBrush)
        return false;

    const D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(ToD2DRect(frame),
        radius, radius);
    outerBrush->SetOpacity(0.24f);
    ctx->DrawRoundedRectangle(outer, outerBrush.Get(), strokeWidth + 1.35f);
    outerBrush->SetOpacity(1.0f);
    ctx->DrawRoundedRectangle(outer, outerBrush.Get(), strokeWidth);

    const float inset = std::max(0.85f, strokeWidth * 0.85f);
    const D2D1_RECT_F innerRect = D2D1::RectF(
        frame.left + inset, frame.top + inset,
        frame.right - inset, frame.bottom - inset);
    if (innerRect.right > innerRect.left && innerRect.bottom > innerRect.top)
    {
        const float darkAlpha = std::clamp(color.a * 0.52f, 0.025f, 0.24f);
        ComPtr<ID2D1SolidColorBrush> innerBrush;
        if (SUCCEEDED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, darkAlpha),
                &innerBrush)) && innerBrush)
        {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(innerRect,
                std::max(0.0f, radius - inset),
                std::max(0.0f, radius - inset)), innerBrush.Get(),
                std::max(0.65f, strokeWidth * 0.65f));
        }
    }
    return true;
}

/** @brief 返回设置界面显示的原生毛玻璃合成状态。 */
inline std::wstring DesktopApp::GetGlassBackendStatusText() const
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
