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
#include "widget_chrome_rules.h"
#include "types.h"
#include "app.h"
#include "widget_preview_scene.h"

namespace
{
const std::unordered_map<std::string, std::string>
    kEmptyPreviewStorage;
}

ID2D1RoundedRectangleGeometry* LuaScript::GetCachedClipGeometry(
    ID2D1Factory1* factory, const RECT& frame, float radius)
{
    if (!factory) return nullptr;
    if (cachedClipGeometry_ &&
        cachedClipFrame_.left == frame.left &&
        cachedClipFrame_.top == frame.top &&
        cachedClipFrame_.right == frame.right &&
        cachedClipFrame_.bottom == frame.bottom &&
        cachedClipRadius_ == radius)
        return cachedClipGeometry_.Get();

    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geo;
    if (FAILED(factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
                    static_cast<float>(frame.right), static_cast<float>(frame.bottom)),
                radius, radius), &geo)) || !geo)
        return nullptr;
    cachedClipGeometry_ = std::move(geo);
    cachedClipFrame_ = frame;
    cachedClipRadius_ = radius;
    return cachedClipGeometry_.Get();
}

LuaScript::WidgetLoadResult LuaScript::SafeLoadWidget(WidgetEngine* engine,
    const std::wstring& id, const std::wstring& scriptPath, bool preview)
{
    WidgetLoadResult result;
    if (!engine) return result;
    __try
    {
        if (preview)
            result.ok = engine->EnsureWidgetPreviewLoaded(
                id, scriptPath, kEmptyPreviewStorage);
        else
            result.ok = engine->EnsureWidgetLoaded(id, scriptPath);
        result.customStyle = engine->HasCustomStyle(id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OutputDebugStringA("SnowDesktop: LuaScript::Draw widget init crash\n");
    }
    return result;
}

bool LuaScript::SafeRenderWidget(const std::wstring& id, const std::wstring& scriptPath,
    WidgetEngine* engine, ID2D1DeviceContext* context, RECT frame,
    int columns, int rows)
{
    if (!engine) return false;
    bool rendered = false;
    __try
    {
        engine->RenderWidget(id, scriptPath, context, frame, columns, rows);
        rendered = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OutputDebugStringA("SnowDesktop: LuaScript::Draw RenderWidget crash\n");
    }
    return rendered;
}

bool LuaScript::SafeReadFlags(WidgetEngine* engine,
    const std::wstring& scriptPath, bool& showTitle, bool& bottomBarHover)
{
    if (!engine) return false;
    __try
    {
        showTitle = engine->ReadBoolFlag(scriptPath, "showTitle", false);
        bottomBarHover = engine->ReadBoolFlag(scriptPath, "bottomBarHover", true);
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
 *   8. 按组件底栏与悬停状态决定是否绘制底栏和缩放按钮；
 *   9. 若底栏可见，绘制底部渐变条（无自定义样式时）；
 *  10. 若 showTitle 为 true 且存在标题文本，则绘制控件标题；
 *  11. 绘制右下角的缩放手柄（圆角小方块）。
 *
 * @param context  Direct2D 设备上下文，用于所有绘制调用
 * @param rect     控件的原始矩形区域（未使用，实际采用 GetStandaloneWidgetFrameRect）
 * @param state    额外状态标记（state == 2 时强制视为选中态）
 */
void LuaScript::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    DrawInternal(context, rect, state,
        app_ ? app_->widgetEngine_.get() : nullptr, false);
}

void LuaScript::DrawPreview(ID2D1DeviceContext* context, RECT frame,
    const snowdesktop::WidgetRenderOptions& options)
{
    const auto* previous = renderOptions_;
    renderOptions_ = &options;
    DrawInternal(context, frame, 0, options.luaEngine, true);
    renderOptions_ = previous;
}

void LuaScript::DrawInternal(ID2D1DeviceContext* context, RECT rect,
    int state, WidgetEngine* engine, bool preview)
{
    if (!context || !data_ || !app_) return;

    RECT frame = preview ? rect : app_->GetStandaloneWidgetFrameRect(*data_);
    if (IsRectEmptyRect(frame)) return;

    const bool selected = data_->selected || state == 2;
    const POINT renderPointer = preview
        ? GetRenderPointer() : app_->lastMousePoint_;
    const bool hovered = PtInRect(&frame, renderPointer) != FALSE;
    const bool lightTheme = app_->IsLightContentTheme();
    int globalContentTheme = 0;
    if (app_->settingsWindow_)
        globalContentTheme = app_->settingsWindow_->GetPersonalization().contentTheme;

    D2D1::ColorF fillColor(0.08f, 0.10f, 0.13f, 0.36f);
    D2D1::ColorF borderColor(1.0f, 1.0f, 1.0f, 0.40f);
    float gradientEndA = 0.65f;
    float cornerRadiusCu = 12.0f;
    PersonalizationSettings effectSettings = PersonalizationSettings::DarkPreset();
    if (app_->settingsWindow_)
    {
        effectSettings = app_->settingsWindow_->GetPersonalization();
        fillColor = D2D1::ColorF(effectSettings.widgetBgR, effectSettings.widgetBgG,
            effectSettings.widgetBgB, effectSettings.widgetAlpha);
        borderColor = D2D1::ColorF(effectSettings.widgetBorderR, effectSettings.widgetBorderG,
            effectSettings.widgetBorderB, effectSettings.widgetBorderAlpha);
        gradientEndA = effectSettings.gradientEndA;
        cornerRadiusCu = effectSettings.cornerRadius;
    }

    bool customStyle = false;
    bool widgetOk = false;
    if (engine)
    {
        auto loadResult = SafeLoadWidget(
            engine, data_->id, data_->packageId, preview);
        widgetOk = loadResult.ok;
        customStyle = loadResult.customStyle;

        if (customStyle && widgetOk)
        {
            std::string fp = engine->RuntimeGetStorageValue(data_->id, "followPersonalization");
            if (fp == "1" || fp == "true")
                customStyle = false;
        }

        if (customStyle && widgetOk)
        {
            effectSettings = PersonalizationSettings::DarkPreset();
            float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f, alpha = 0.0f;
            float borderR = 0.0f, borderG = 0.0f, borderB = 0.0f, borderAlpha = 0.0f;
            float luaGradientEndA = gradientEndA;
            bool luaGlassEnabled = false;
            bool luaAcrylicEnabled = false;
            if (engine->ReadCustomColors(data_->id,
                bgR, bgG, bgB, alpha, borderR, borderG, borderB, borderAlpha,
                luaGradientEndA, luaGlassEnabled, luaAcrylicEnabled))
            {
                fillColor = D2D1::ColorF(bgR, bgG, bgB, alpha);
                borderColor = D2D1::ColorF(borderR, borderG, borderB, borderAlpha);
                gradientEndA = luaGradientEndA;
                effectSettings = PersonalizationSettings::DarkPreset();
                effectSettings.glassEnabled = luaGlassEnabled;
                effectSettings.acrylicEnabled =
                    luaGlassEnabled && luaAcrylicEnabled;
            }
        }

        // 所有面板共享原生模糊半径；Lua 仅保留实例级毛玻璃开关。
        // 自定义风格组件保留文字颜色设置：优先组件级存储，其次使用全局值
        if (customStyle && app_->settingsWindow_)
        {
            int ct = globalContentTheme;
            std::string stored = engine->RuntimeGetStorageValue(data_->id, "__contentTheme");
            if (!stored.empty())
                ct = std::clamp(std::stoi(stored), 0, 1);
            effectSettings.contentTheme = ct;
        }
        if (app_->settingsWindow_)
        {
            const auto& global = app_->settingsWindow_->GetPersonalization();
            effectSettings.glassBlurRadius = global.glassBlurRadius;
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
            theme.borderAlpha = borderColor.a;
            theme.gradientEndA = gradientEndA;
            theme.cornerRadius = cornerRadiusCu;
            theme.contentTheme = effectSettings.contentTheme;
            engine->SetWidgetTheme(data_->id, theme);
        }
    }

    app_->DrawWidgetPanelBackground(context, frame, static_cast<float>(Cu(cornerRadiusCu)),
        fillColor, borderColor, selected, selected ? 1.6f : 1.0f,
        customStyle ? &effectSettings : nullptr, !preview);

    context->PushAxisAlignedClip(app_->ToD2DRect(frame), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (engine && widgetOk)
    {
        const POINT center = { (frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2 };
        const GridPage* realPage = nullptr;
        if (preview)
        {
            realPage = FindGridPage(
                app_->gridPages_, data_->gridCell.pageId);
            int cellWidth = 0;
            int cellHeight = 0;
            int gapY = 0;
            if (realPage)
            {
                cellWidth = realPage->cellWidth;
                cellHeight = realPage->cellHeight;
                gapY = realPage->gapY;
            }
            else
            {
                cellWidth =
                    std::max(1, static_cast<int>(frame.right - frame.left) /
                        std::max(1, data_->gridSpan.columns));
                cellHeight =
                    std::max(1, static_cast<int>(frame.bottom - frame.top) /
                        std::max(1, data_->gridSpan.rows));
                gapY = Cu(8.0f);
            }
            engine->SetWidgetLayoutMetrics(data_->id,
                cellWidth, cellHeight, gapY,
                static_cast<int>(GetBarHeight()),
                app_->GetItemFontWeight());
        }
        else
        {
            for (const auto& p : app_->gridPages_)
            {
                if (PtInRect(&p.bounds, center)) { realPage = &p; break; }
            }
            if (!realPage)
                realPage = FindGridPage(app_->gridPages_, data_->gridCell.pageId);
            if (realPage)
            {
                engine->SetWidgetLayoutMetrics(data_->id,
                    realPage->cellWidth, realPage->cellHeight,
                    realPage->gapY,
                    static_cast<int>(GetBarHeight()),
                    app_->GetItemFontWeight());
                if (data_->gridCell.pageId != realPage->id)
                {
                    data_->gridCell.pageId = realPage->id;
                    RECT correctBounds = GetGridRect(app_->gridPages_, data_->gridCell, data_->gridSpan);
                    int hgx = std::max(Cu(2.0f), realPage->gapX / 2);
                    int hgy = std::max(Cu(2.0f), realPage->gapY / 2);
                    frame = correctBounds;
                    frame.left   -= hgx; frame.top    -= hgy;
                    frame.right  += hgx; frame.bottom += hgy;
                    const int inset = Cu(4.0f);
                    if (frame.right - frame.left > inset * 4 && frame.bottom - frame.top > inset * 4)
                        InflateRect(&frame, -inset, -inset);
                }
            }
        }
        LuaWidgetSurfaceContext surfaceContext;
        if (preview)
        {
            const UINT previewDpi = renderOptions_
                ? renderOptions_->dpi : USER_DEFAULT_SCREEN_DPI;
            surfaceContext.dpiX = previewDpi;
            surfaceContext.dpiY = previewDpi;
            surfaceContext.monitorBounds = frame;
            surfaceContext.workArea = frame;
        }
        else if (realPage)
        {
            surfaceContext.dpiX = realPage->dpiX;
            surfaceContext.dpiY = realPage->dpiY;
            surfaceContext.monitorBounds = realPage->bounds;
            surfaceContext.workArea = realPage->workArea;
            surfaceContext.monitorAvailable = true;
            surfaceContext.primaryMonitor = realPage->isPrimary;
        }
        engine->SetWidgetSurfaceContext(data_->id, surfaceContext);
        widgetOk = SafeRenderWidget(
            data_->id, data_->packageId, engine, context, frame,
            data_->gridSpan.columns, data_->gridSpan.rows);
        if (widgetOk)
        {
            widgetOk = engine->GetWidgetHostState(
                data_->id, data_->packageId).kind ==
                snowdesktop::widget_runtime::WidgetHostStateKind::Ready;
        }
    }
    if (!preview && !widgetOk &&
        !WidgetEngine::IsWidgetPackageInstalled(data_->packageId))
    {
        const std::string publishedFileId =
            data_->packageSourceProvider == L"steam-workshop"
            ? snowdesktop::widget::SteamPublishedFileId(
                WideToUtf8(data_->packageSourceExternalItemId))
            : std::string{};
        const bool canRecoverFromWorkshop = !publishedFileId.empty();
        const int centerY = frame.top +
            (frame.bottom - frame.top) / 2;
        const int horizontalPadding = Cu(14.0f);
        RECT titleRect{
            frame.left + horizontalPadding,
            centerY - Cu(36.0f),
            frame.right - horizontalPadding,
            centerY - Cu(7.0f),
        };
        const std::wstring title = data_->title.empty()
            ? _LW("app.widget.missing_component")
            : data_->title;
        app_->DrawD2DText(context, title, titleRect,
            GetCuTextFormatWeight(16.0f,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, true),
            lightTheme
                ? D2D1::ColorF(0.09f, 0.12f, 0.17f, 0.92f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f));

        const RECT actionRect =
            app_->GetLuaWidgetHostActionRect(*data_);
        const bool actionHovered =
            PtInRect(&actionRect, renderPointer) != FALSE;
        if (canRecoverFromWorkshop)
        {
            app_->DrawD2DRoundedRectangle(context, actionRect,
                static_cast<float>(Cu(8.0f)),
                actionHovered
                    ? D2D1::ColorF(0.22f, 0.50f, 0.96f, 0.28f)
                    : D2D1::ColorF(0.22f, 0.50f, 0.96f, 0.16f),
                D2D1::ColorF(0.30f, 0.58f, 1.0f,
                    actionHovered ? 0.72f : 0.46f));
        }
        app_->DrawD2DText(context,
            canRecoverFromWorkshop
                ? _LW("app.widget.missing_workshop_action")
                : _LW("app.widget.missing_local_hint"),
            actionRect,
            GetCuTextFormatWeight(12.5f,
                canRecoverFromWorkshop
                    ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                    : DWRITE_FONT_WEIGHT_NORMAL,
                true),
            canRecoverFromWorkshop
                ? (lightTheme
                    ? D2D1::ColorF(0.08f, 0.31f, 0.72f, 0.98f)
                    : D2D1::ColorF(0.63f, 0.79f, 1.0f, 0.98f))
                : (lightTheme
                    ? D2D1::ColorF(0.20f, 0.23f, 0.29f, 0.68f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.64f)),
            DWRITE_WORD_WRAPPING_WRAP);
    }
    else if (!preview && !widgetOk)
    {
        using snowdesktop::widget_runtime::WidgetHostStateKind;
        const auto hostState = engine
            ? engine->GetWidgetHostState(data_->id, data_->packageId)
            : snowdesktop::widget_runtime::WidgetHostState{
                WidgetHostStateKind::LoadFailed, {} };
        std::wstring title;
        std::wstring body;
        std::wstring action;
        switch (hostState.kind)
        {
        case WidgetHostStateKind::PermissionPending:
            title = _LW("app.widget.placeholder.permission_title");
            body = _LW("app.widget.placeholder.permission_pending");
            action = _LW("app.widget.placeholder.reauthorize");
            break;
        case WidgetHostStateKind::PermissionDenied:
            title = _LW("app.widget.placeholder.permission_title");
            body = _LW("app.widget.placeholder.permission_denied");
            action = _LW("app.widget.placeholder.reauthorize");
            break;
        case WidgetHostStateKind::QuotaExceeded:
            title = _LW("app.widget.placeholder.quota_title");
            body = _LW("app.widget.placeholder.quota_body");
            action = _LW("app.widget.placeholder.reload");
            break;
        case WidgetHostStateKind::RuntimeSuspended:
            title = _LW("app.widget.placeholder.runtime_title");
            body = _LW("app.widget.placeholder.runtime_suspended");
            action = _LW("app.widget.placeholder.reload");
            break;
        case WidgetHostStateKind::LoadFailed:
        default:
            title = _LW("app.widget.placeholder.runtime_title");
            body = _LW("app.widget.placeholder.load_failed");
            action = _LW("app.widget.placeholder.reload");
            break;
        }

        const int horizontalPadding = Cu(14.0f);
        const RECT actionRect =
            app_->GetLuaWidgetHostActionRect(*data_);
        const bool actionHovered =
            PtInRect(&actionRect, renderPointer) != FALSE;
        RECT titleRect{
            frame.left + horizontalPadding,
            frame.top + Cu(13.0f),
            frame.right - horizontalPadding,
            std::min<LONG>(actionRect.top - Cu(4.0f),
                frame.top + Cu(39.0f)),
        };
        RECT bodyRect{
            titleRect.left,
            titleRect.bottom + Cu(2.0f),
            titleRect.right,
            actionRect.top - Cu(7.0f),
        };
        if (titleRect.bottom > titleRect.top)
        {
            app_->DrawD2DText(context, title, titleRect,
                GetCuTextFormatWeight(16.0f,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, false),
                lightTheme
                    ? D2D1::ColorF(0.09f, 0.12f, 0.17f, 0.94f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        }
        if (bodyRect.bottom > bodyRect.top)
        {
            app_->DrawD2DText(context, body, bodyRect,
                GetCuTextFormatWeight(12.0f,
                    DWRITE_FONT_WEIGHT_NORMAL, false),
                lightTheme
                    ? D2D1::ColorF(0.20f, 0.23f, 0.29f, 0.72f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.68f),
                DWRITE_WORD_WRAPPING_WRAP);
        }
        app_->DrawD2DRoundedRectangle(context, actionRect,
            static_cast<float>(Cu(8.0f)),
            actionHovered
                ? D2D1::ColorF(0.22f, 0.50f, 0.96f, 0.28f)
                : D2D1::ColorF(0.22f, 0.50f, 0.96f, 0.16f),
            D2D1::ColorF(0.30f, 0.58f, 1.0f,
                actionHovered ? 0.72f : 0.46f));
        app_->DrawD2DText(context, action, actionRect,
            GetCuTextFormatWeight(12.5f,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, true),
            lightTheme
                ? D2D1::ColorF(0.08f, 0.31f, 0.72f, 0.98f)
                : D2D1::ColorF(0.63f, 0.79f, 1.0f, 0.98f));
    }
    context->PopAxisAlignedClip();

    if (engine && widgetOk)
        SafeReadFlags(engine, data_->packageId,
            data_->showTitle, data_->bottomBarHover);

    if (engine && widgetOk)
    {
        auto scrollControls = engine->GetScrollControls(data_->id);
        for (const auto& ctrl : scrollControls)
        {
            const int contentExtent = ctrl.horizontal
                ? ctrl.contentWidth : ctrl.contentHeight;
            const int viewportExtent = ctrl.horizontal
                ? ctrl.viewportWidth : ctrl.viewportHeight;
            if (contentExtent <= viewportExtent) continue;
            const RECT viewport = {
                frame.left + ctrl.rect.left,
                frame.top + ctrl.rect.top,
                frame.left + ctrl.rect.right,
                frame.top + ctrl.rect.bottom
            };
            const int scrollOff = engine->RuntimeGetScrollOffset(
                data_->id, ctrl.id);
            const bool showScrollbar = hovered || !data_->bottomBarHover;
            if (ctrl.horizontal)
                DrawHorizontalScrollbarAt(context, viewport,
                    ctrl.contentWidth, ctrl.viewportWidth, scrollOff,
                    showScrollbar, app_->IsLightContentTheme(),
                    GetCellScale());
            else
                DrawScrollbarAt(context, viewport,
                    ctrl.contentHeight, ctrl.viewportHeight, scrollOff,
                    showScrollbar, app_->IsLightContentTheme(),
                    GetCellScale());
        }
    }

    const bool showBottomBar = snowdesktop::widget_chrome_rules::ShowsBottomBar(
        data_->showTitle, data_->bottomBarHover, hovered);
    const bool showResizeHandle =
        snowdesktop::widget_chrome_rules::ShowsResizeHandle(
            data_->showTitle, data_->bottomBarHover, hovered);
    if (!showBottomBar && !showResizeHandle) return;

    if (showBottomBar)
    {
        RECT handle = app_->GetStandaloneWidgetMoveHandleRect(*data_);
        RECT gradientRect = { frame.left, std::max<LONG>(frame.top, frame.bottom - Cu(36.0f)),
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
                    const float radius = static_cast<float>(Cu(12.0f));
                    ID2D1RoundedRectangleGeometry* clipGeo = GetCachedClipGeometry(factory, frame, radius);
                    bool pushed = false;
                    if (clipGeo)
                    {
                        context->PushLayer(D2D1::LayerParameters(
                            D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
                                static_cast<float>(frame.right), static_cast<float>(frame.bottom)),
                            clipGeo), nullptr);
                        pushed = true;
                    }
                    context->FillRectangle(app_->ToD2DRect(gradientRect), brush.Get());
                    if (pushed)
                        context->PopLayer();
                }
            }
        }

        if (!data_->title.empty())
        {
            const float bh = GetBarHeight();
            RECT titleRect = {
                handle.left + Cu(4.0f),
                handle.top + Cu(bh * 0.083f),
                std::max<LONG>(handle.left + Cu(5.0f), handle.right - Cu(bh * 1.17f)),
                handle.bottom - Cu(bh * 0.083f)
            };
            auto titleWeight = static_cast<DWRITE_FONT_WEIGHT>(
                std::max<int>(100, static_cast<int>(app_->GetItemFontWeight()) - (lightTheme ? 200 : 0)));
            IDWriteTextFormat* titleFormat = GetCuTextFormatWeight(bh * 0.542f, titleWeight, false);
            app_->DrawD2DText(context, data_->title, titleRect,
                titleFormat ? titleFormat : app_->listItemTextFormat_.Get(),
                lightTheme
                    ? D2D1::ColorF(0.11f, 0.13f, 0.17f, 0.96f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
        }
    }

    if (!showResizeHandle) return;
    RECT resize = app_->GetStandaloneWidgetResizeHandleRect(*data_);
    const int dot = Cu(GetBarHeight() * 0.333f);
    int cx = resize.left + (resize.right - resize.left) / 2;
    int cy = resize.top + (resize.bottom - resize.top) / 2;
    RECT dotRect = { cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2 };
    app_->DrawD2DRoundedRectangle(context, dotRect, static_cast<float>(Cu(4.0f * GetBarScale())),
        selected ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.62f)
                 : (lightTheme
                    ? D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.34f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f)),
        lightTheme
            ? D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.50f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f));
}
