#include "widget.h"
#include "types.h"
#include "app.h"

void LuaScript::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    (void)rect;
    if (!context || !data_ || !app_) return;

    RECT frame = app_->GetStandaloneWidgetFrameRect(*data_);
    if (IsRectEmptyRect(frame)) return;

    const bool selected = data_->selected || state == 2;
    const bool hovered = PtInRect(&frame, app_->lastMousePoint_) != FALSE;

    D2D1::ColorF fillColor(0.08f, 0.10f, 0.13f, 0.36f);
    D2D1::ColorF borderColor(1.0f, 1.0f, 1.0f, 0.40f);
    float gradientEndA = 0.65f;
    if (app_->settingsWindow_)
    {
        const auto& p = app_->settingsWindow_->GetPersonalization();
        fillColor = D2D1::ColorF(p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
        borderColor = D2D1::ColorF(p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetAlpha);
        gradientEndA = p.gradientEndA;
    }

    bool customStyle = false;
    if (app_->widgetEngine_)
    {
        app_->widgetEngine_->EnsureWidgetLoaded(data_->id, data_->scriptPath);
        customStyle = app_->widgetEngine_->HasCustomStyle(data_->id);
        if (customStyle)
        {
            float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f, alpha = 0.0f;
            float borderR = 0.0f, borderG = 0.0f, borderB = 0.0f, luaGradientEndA = gradientEndA;
            if (app_->widgetEngine_->ReadCustomColors(data_->id,
                bgR, bgG, bgB, alpha, borderR, borderG, borderB, luaGradientEndA))
            {
                fillColor = D2D1::ColorF(bgR, bgG, bgB, alpha);
                borderColor = D2D1::ColorF(borderR, borderG, borderB, alpha);
                gradientEndA = luaGradientEndA;
            }
        }

        auto colorToRgb = [](const D2D1::ColorF& color) {
            auto toByte = [](float v) {
                v = std::max(0.0f, std::min(1.0f, v));
                return static_cast<int>(v * 255.0f + 0.5f);
            };
            return (toByte(color.r) << 16) | (toByte(color.g) << 8) | toByte(color.b);
        };
        LuaWidgetTheme theme;
        theme.bg = colorToRgb(fillColor);
        theme.border = colorToRgb(borderColor);
        theme.alpha = fillColor.a;
        theme.gradientEndA = gradientEndA;
        app_->widgetEngine_->SetWidgetTheme(data_->id, theme);
    }

    app_->DrawD2DRoundedRectangle(context, frame, 12.0f, fillColor,
        selected ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.90f) : borderColor,
        selected ? 1.6f : 1.0f);

    context->PushAxisAlignedClip(app_->ToD2DRect(frame), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (app_->widgetEngine_)
        app_->widgetEngine_->RenderWidget(data_->id, data_->scriptPath, context, frame);
    context->PopAxisAlignedClip();

    bool showHandle = data_->bottomBarHover ? hovered : true;
    if (!showHandle) return;

    RECT handle = app_->GetStandaloneWidgetMoveHandleRect(*data_);
    RECT gradientRect = { frame.left, std::max<LONG>(frame.top, frame.bottom - 36),
                          frame.right, frame.bottom };
    if (!customStyle && gradientRect.bottom > gradientRect.top)
    {
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP sd[] = {
            { 0.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, 0.0f) },
            { 1.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, gradientEndA) },
        };
        if (SUCCEEDED(context->CreateGradientStopCollection(sd, 2,
            D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops)) && stops)
        {
            Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
            if (SUCCEEDED(context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0.0f, static_cast<float>(gradientRect.top)),
                    D2D1::Point2F(0.0f, static_cast<float>(gradientRect.bottom))),
                stops.Get(), &brush)) && brush)
            {
                context->FillRectangle(app_->ToD2DRect(gradientRect), brush.Get());
            }
        }
    }

    if (data_->showTitle && !data_->title.empty())
    {
        RECT titleRect = {
            handle.left + 4,
            handle.top + 2,
            std::max<LONG>(handle.left + 5, handle.right - 28),
            handle.bottom - 2
        };
        app_->DrawD2DText(context, data_->title, titleRect,
            app_->listItemTextFormat_.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
    }

    RECT resize = app_->GetStandaloneWidgetResizeHandleRect(*data_);
    const int dot = 8;
    int cx = resize.left + (resize.right - resize.left) / 2;
    int cy = resize.top + (resize.bottom - resize.top) / 2;
    RECT dotRect = { cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2 };
    app_->DrawD2DRoundedRectangle(context, dotRect, 4.0f,
        selected ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.62f)
                 : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f));
}
