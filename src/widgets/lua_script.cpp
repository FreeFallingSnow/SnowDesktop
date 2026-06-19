/**
 * @file lua_script.cpp
 * @brief LuaScript 控件实现
 *
 * LuaScript 是一个纯渲染控件，由 Lua 脚本驱动绘制逻辑。
 * 它不包含任何 Chrome 元素（窗口装饰、标题栏等），也不具备容器能力，
 * 完全依靠脚本引擎提供的自定义样式和渲染内容来呈现。
 * 同时负责处理选中态、悬停态、渐变底色、标题显示、缩放手柄等交互细节。
 */

#include "widget.h"
#include "types.h"
#include "app.h"

LuaScript::WidgetLoadResult LuaScript::SafeLoadWidget(const std::wstring& id, const std::wstring& scriptPath)
{
    WidgetLoadResult result;
    __try
    {
        app_->widgetEngine_->EnsureWidgetLoaded(id, scriptPath);
        result.customStyle = app_->widgetEngine_->HasCustomStyle(id);
        result.ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OutputDebugStringA("SnowDesktop: LuaScript::Draw widget init crash\n");
    }
    return result;
}

bool LuaScript::SafeRenderWidget(const std::wstring& id, const std::wstring& scriptPath,
    ID2D1DeviceContext* context, RECT frame, int columns, int rows)
{
    __try
    {
        app_->widgetEngine_->RenderWidget(id, scriptPath, context, frame, columns, rows);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OutputDebugStringA("SnowDesktop: LuaScript::Draw RenderWidget crash\n");
        return false;
    }
}

bool LuaScript::SafeReadFlags(const std::wstring& scriptPath, bool& showTitle, bool& bottomBarHover)
{
    __try
    {
        showTitle = app_->widgetEngine_->ReadBoolFlag(scriptPath, "showTitle", false);
        bottomBarHover = app_->widgetEngine_->ReadBoolFlag(scriptPath, "bottomBarHover", true);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        showTitle = false;
        bottomBarHover = true;
        return false;
    }
}

/**
 * @brief 绘制 LuaScript 控件
 *
 * 本函数完成以下绘制流程：
 *   1. 获取控件窗口矩形并进行空区域检测；
 *   2. 根据选中/悬停状态选取填充色与边框色，支持从 Personalization 配置读取；
 *   3. 检测 Lua 脚本是否定义了自定义样式（CustomStyle），若有则覆盖默认颜色；
 *   4. 构造 LuaWidgetTheme 结构体并传递给脚本引擎，供 Lua 渲染时参考；
 *   5. 绘制圆角矩形背景与选中边框；
 *   6. 设置裁剪区域，调用脚本引擎的 RenderWidget 执行 Lua 自定义绘制；
 *   7. 从脚本读取 showTitle / bottomBarHover 等标志位；
 *   8. 若不总是显示底部栏，则在非悬停时提前返回；
 *   9. 绘制底部渐变条（无自定义样式时）；
 *  10. 若 showTitle 为 true 且存在标题文本，则绘制控件标题；
 *  11. 绘制右下角的缩放手柄（圆角小方块）。
 *
 * @param context  Direct2D 设备上下文，用于所有绘制调用
 * @param rect     控件的原始矩形区域（未使用，实际采用 GetStandaloneWidgetFrameRect）
 * @param state    额外状态标记（state == 2 时强制视为选中态）
 */
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
    bool widgetOk = false;
    if (app_->widgetEngine_)
    {
        auto loadResult = SafeLoadWidget(data_->id, data_->scriptPath);
        widgetOk = loadResult.ok;
        customStyle = loadResult.customStyle;

        if (customStyle && widgetOk)
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

        if (widgetOk)
        {
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
    }

    app_->DrawD2DRoundedRectangle(context, frame, 12.0f, fillColor,
        selected ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.90f) : borderColor,
        selected ? 1.6f : 1.0f);

    context->PushAxisAlignedClip(app_->ToD2DRect(frame), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (app_->widgetEngine_ && widgetOk)
    {
        const POINT center = { (frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2 };
        const GridPage* realPage = nullptr;
        for (const auto& p : app_->gridPages_)
        {
            if (PtInRect(&p.bounds, center)) { realPage = &p; break; }
        }
        if (!realPage)
            realPage = FindGridPage(app_->gridPages_, data_->gridCell.pageId);
        if (realPage)
        {
            app_->widgetEngine_->SetGridCellSize(realPage->cellWidth, realPage->cellHeight);
            app_->widgetEngine_->SetGridCellGap(realPage->gapY);
            if (data_->gridCell.pageId != realPage->id)
            {
                data_->gridCell.pageId = realPage->id;
                RECT correctBounds = GetGridRect(app_->gridPages_, data_->gridCell, data_->gridSpan);
                int hgx = std::max(2, realPage->gapX / 2);
                int hgy = std::max(2, realPage->gapY / 2);
                frame = correctBounds;
                frame.left   -= hgx; frame.top    -= hgy;
                frame.right  += hgx; frame.bottom += hgy;
                if (frame.right - frame.left > 16 && frame.bottom - frame.top > 16)
                    InflateRect(&frame, -4, -4);
            }
        }
        SafeRenderWidget(data_->id, data_->scriptPath, context, frame,
            data_->gridSpan.columns, data_->gridSpan.rows);
    }
    context->PopAxisAlignedClip();

    if (app_->widgetEngine_ && widgetOk)
        SafeReadFlags(data_->scriptPath, data_->showTitle, data_->bottomBarHover);

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
                auto* factory = app_->GetD2DFactory();
                Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
                bool pushed = false;
                if (factory && SUCCEEDED(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(
                        D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
                            static_cast<float>(frame.right), static_cast<float>(frame.bottom)),
                        12.0f, 12.0f), &clipGeo)) && clipGeo)
                {
                    context->PushLayer(D2D1::LayerParameters(
                        D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
                            static_cast<float>(frame.right), static_cast<float>(frame.bottom)),
                        clipGeo.Get()), nullptr);
                    pushed = true;
                }
                context->FillRectangle(app_->ToD2DRect(gradientRect), brush.Get());
                if (pushed)
                    context->PopLayer();
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
