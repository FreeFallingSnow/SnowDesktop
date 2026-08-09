#include "app.h"
#include "quick_navigation_helpers.h"
#include "quick_navigation_rules.h"
#include "quick_navigation_theme.h"

// Quick-navigation DirectWrite and DirectComposition rendering resources.

void DesktopApp::EnsureQuickNavTextFormats()
{
    if (!dwriteFactory_)
        return;

    const float tabSize = static_cast<float>(QuickNavScale(14));
    const float itemSize = static_cast<float>(QuickNavScale(13));
    const float pathSize = static_cast<float>(QuickNavScale(11));
    // 深色用极细字重：Segoe UI Variable 支持连续字重轴，100=Thin 才能真正比 Light(300) 更细。
    const float itemWeightValue = quickNavLightTheme_ ? 550.0f : 100.0f;
    const DWRITE_FONT_WEIGHT itemWeight =
        static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(itemWeightValue));

    auto createOrRecreate = [&](ComPtr<IDWriteTextFormat>& fmt, const wchar_t* family,
        float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT hAlign) {
        const bool stale = !fmt ||
            std::abs(fmt->GetFontSize() - size) > 0.01f ||
            fmt->GetFontWeight() != weight;
        if (stale)
        {
            fmt.Reset();
            dwriteFactory_->CreateTextFormat(family, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
            if (fmt)
            {
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                fmt->SetTextAlignment(hAlign);
            }
        }
    };

    createOrRecreate(quickNavTabTextFormat_, L"Segoe UI", tabSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    createOrRecreate(quickNavItemTextFormat_, L"Segoe UI Variable", itemSize, itemWeight, DWRITE_TEXT_ALIGNMENT_LEADING);
    createOrRecreate(quickNavPathTextFormat_, L"Segoe UI", pathSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!quickNavFluentTextFormat_ ||
        std::abs(
            quickNavFluentTextFormat_->GetFontSize() -
                tabSize) > 0.01f)
    {
        quickNavFluentTextFormat_.Reset();
        quickNavFluentTextFormat_ =
            ComPtr<IDWriteTextFormat>(
                CreateFluentTextFormat(
                    dwriteFactory_.Get(),
                    tabSize));
        if (quickNavFluentTextFormat_)
        {
            quickNavFluentTextFormat_->
                SetTextAlignment(
                    DWRITE_TEXT_ALIGNMENT_CENTER);
            quickNavFluentTextFormat_->
                SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            quickNavFluentTextFormat_->
                SetWordWrapping(
                    DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    ComPtr<IDWriteTextFormat3> itemFormat3;
    if (quickNavItemTextFormat_ && SUCCEEDED(quickNavItemTextFormat_.As(&itemFormat3)) && itemFormat3)
    {
        DWRITE_FONT_AXIS_VALUE axes[] = {
            { DWRITE_FONT_AXIS_TAG_WEIGHT, itemWeightValue }
        };
        itemFormat3->SetFontAxisValues(axes, ARRAYSIZE(axes));
    }
}

/**
 * @brief 重置快捷导航 DComp 表面与渲染缓存（设备丢失或窗口销毁时调用）。
 */
void DesktopApp::ResetQuickNavCompositionResources()
{
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    quickNavSysIconCache_.clear();
    if (quickNavDcompVisual_)
        quickNavDcompVisual_->SetContent(nullptr);
    quickNavDcompSurface_.Reset();
    quickNavCompWidth_ = 0;
    quickNavCompHeight_ = 0;
}

/**
 * @brief 快捷导航 DComp 渲染失败后重置表面并安排一次恢复重绘。
 */
void DesktopApp::RecoverQuickNavCompositionFailure(const wchar_t* stage, HRESULT hr)
{
    wchar_t buf[192];
    wsprintfW(buf, L"QuickNav %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render", static_cast<unsigned>(hr));
    WriteDiagnosticLogEntry(buf);

    ResetQuickNavCompositionResources();

    if (!quickNavCompositionRenderRecoveryPending_ && quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
    {
        quickNavCompositionRenderRecoveryPending_ = true;
        InvalidateRect(quickNavigationHwnd_, nullptr, FALSE);
    }
}

/**
 * @brief 创建或调整快捷导航 DComp 表面大小。
 * @return S_OK 成功，否则为 HRESULT 错误码
 */
HRESULT DesktopApp::CreateOrResizeQuickNavCompositionSurface()
{
    if (!dcompDevice_ || !quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return E_UNEXPECTED;

    const UINT width = static_cast<UINT>(
        std::max<LONG>(
            1,
            quickNavigationRect_.right -
                quickNavigationRect_.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(
            1,
            quickNavigationRect_.bottom -
                quickNavigationRect_.top));

    if (!quickNavDcompTarget_)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(quickNavigationHwnd_, FALSE, &quickNavDcompTarget_);
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"QuickNav CreateTargetForHwnd FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(buf);
            return hr;
        }
    }
    if (!quickNavDcompVisual_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(&quickNavDcompVisual_);
        if (FAILED(hr) || !quickNavDcompVisual_)
        {
            wchar_t buf[128];
            wsprintfW(buf, L"QuickNav CreateVisual FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(buf);
            return hr;
        }
        quickNavDcompTarget_->SetRoot(quickNavDcompVisual_.Get());
    }
    if (!quickNavDcompEffect_)
    {
        HRESULT hr =
            dcompDevice_->CreateEffectGroup(
                &quickNavDcompEffect_);
        if (FAILED(hr) || !quickNavDcompEffect_)
        {
            wchar_t buf[128];
            wsprintfW(
                buf,
                L"QuickNav CreateEffectGroup FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(buf);
            return FAILED(hr) ? hr : E_FAIL;
        }
        hr = quickNavDcompVisual_->SetEffect(
            quickNavDcompEffect_.Get());
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(
                buf,
                L"QuickNav SetEffect FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(buf);
            return hr;
        }
    }

    if (quickNavDcompSurface_ && quickNavCompWidth_ == width && quickNavCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"QuickNav CreateSurface %ux%u FAILED hr=0x%08X", width, height, static_cast<unsigned>(hr));
        WriteDiagnosticLogEntry(buf);
        return hr;
    }
    hr = quickNavDcompVisual_->SetContent(surface.Get());
    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"QuickNav SetContent FAILED hr=0x%08X", static_cast<unsigned>(hr));
        WriteDiagnosticLogEntry(buf);
        return hr;
    }
    hr = dcompDevice_->Commit();
    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"QuickNav CreateSurface Commit FAILED hr=0x%08X", static_cast<unsigned>(hr));
        WriteDiagnosticLogEntry(buf);
        return hr;
    }

    quickNavDcompSurface_ = surface;
    quickNavCompWidth_ = width;
    quickNavCompHeight_ = height;
    return S_OK;
}

/**
 * @brief 绘制快捷导航窗口（含搜索栏、标签页、列表、滚动条）
 * @param hwnd 窗口句柄
 */

void DesktopApp::PaintQuickNavigationWindow(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc)
        return;
    // hdc 仅用于验证绘制区域；实际绘制走 DComp surface。
    (void)hdc;
    if (quickNavCompositionPaintInProgress_)
    {
        EndPaint(hwnd, &ps);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    quickNavCompositionPaintInProgress_ = true;
    struct QuickNavigationPaintScope final
    {
        bool& active;
        ~QuickNavigationPaintScope()
        {
            active = false;
        }
    } paintScope{
        quickNavCompositionPaintInProgress_
    };

    const QuickNavTheme& t = quickNavLightTheme_ ? kQuickNavLight : kQuickNavDark;

    HRESULT hr = CreateOrResizeQuickNavCompositionSurface();
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"CreateOrResizeQuickNavCompositionSurface", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    ApplyQuickNavigationAnimationFrame();

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = quickNavDcompSurface_->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext), &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverQuickNavCompositionFailure(L"BeginDraw", hr);
        EndPaint(hwnd, &ps);
        return;
    }

    ComPtr<ID2D1DeviceContext> ctx;
    ctx.Attach(rawContext);
    ctx->SetDpi(96.0f, 96.0f);
    ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    // 内容 surface 只按目标面板大小分配；顶层宿主可以覆盖完整动画路径。
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(
            updateOffset.x -
            quickNavigationRect_.left),
        static_cast<float>(
            updateOffset.y -
            quickNavigationRect_.top)));
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // 文本抗锯齿沿用 DComp 默认（与桌面一致），避免在 alpha 表面上强制 ClearType 产生彩色毛边。
    ctx->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    brushCache_.clear();
    brushCacheContext_ = ctx.Get();

    const RECT& overlay = quickNavigationRect_;
    const float windowCornerRadius =
        static_cast<float>(
            QuickNavScale(16)) / 2.0f;
    ComPtr<ID2D1RoundedRectangleGeometry>
        windowClipGeometry;
    bool windowClipPushed = false;
    if (d2dFactory_ &&
        SUCCEEDED(
            d2dFactory_->
                CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(
                        ToD2DRect(overlay),
                        windowCornerRadius,
                        windowCornerRadius),
                    &windowClipGeometry)) &&
        windowClipGeometry)
    {
        ctx->PushLayer(
            D2D1::LayerParameters(
                ToD2DRect(overlay),
                windowClipGeometry.Get()),
            nullptr);
        windowClipPushed = true;
    }
    const float windowAlpha = std::clamp(quickNavAppearance_.widgetAlpha, 0.0f, 1.0f);
    const float borderAlpha =
        quickNavigationAnimation_.IsAnimating()
            ? 0.0f
            : std::clamp(
                quickNavAppearance_.
                    widgetBorderAlpha,
                0.0f, 1.0f);
    // 毛玻璃 backdrop 不可用（无合成器支持的环境）时退化为不透明主题色，
    // 避免半透明面板与窗口黑底混合成黑色。
    const float effectiveBgAlpha =
        quickNavAppearance_.glassEnabled &&
            !quickNavBackdropCompositor_.IsAvailable()
        ? std::max(windowAlpha, 0.95f)
        : windowAlpha;
    DrawD2DRoundedRectangle(
        ctx.Get(), overlay,
        windowCornerRadius,
        D2D1::ColorF(
            quickNavAppearance_.widgetBgR,
            quickNavAppearance_.widgetBgG,
            quickNavAppearance_.widgetBgB,
            effectiveBgAlpha),
        D2D1::ColorF(0, 0, 0, 0));
    if (quickNavAppearance_.glassEnabled &&
        quickNavAppearance_.acrylicEnabled)
    {
        POINT screenOrigin{};
        ClientToScreen(quickNavigationHwnd_, &screenOrigin);
        DrawAcrylicNoise(ctx.Get(), overlay,
            static_cast<float>(QuickNavScale(16)) / 2.0f,
            quickNavAppearance_.contentTheme == 1, screenOrigin);
    }
    DrawD2DRoundedRectangle(ctx.Get(),
        MakeRect(overlay.left, overlay.top, overlay.right - 1, overlay.bottom - 1),
        windowCornerRadius,
        D2D1::ColorF(0, 0, 0, 0),
        D2D1::ColorF(quickNavAppearance_.widgetBorderR,
            quickNavAppearance_.widgetBorderG,
            quickNavAppearance_.widgetBorderB, borderAlpha));

    const bool searching = !GetQuickNavigationEffectiveSearchText().empty();
    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    QuickNavigationContentModel contentModel =
        BuildQuickNavigationContentModel();
    const std::vector<QuickNavigationEntry>& entries =
        contentModel.entries;
    quickNavigationHoverRegions_.clear();
    auto registerHoverRegion = [this](
        RECT bounds,
        QuickNavigationPointerTargetKind kind,
        size_t index = 0) {
        if (!IsRectEmpty(&bounds))
            quickNavigationHoverRegions_.push_back(
                { bounds, { kind, index } });
    };
    auto registerClippedHoverRegion =
        [&registerHoverRegion](
            RECT bounds, const RECT& clip,
            QuickNavigationPointerTargetKind kind,
            size_t index = 0) {
            RECT visible{};
            if (IntersectRect(
                    &visible, &bounds, &clip))
                registerHoverRegion(
                    visible, kind, index);
        };
    quickNavigationTabScrollOffset_ = std::clamp(quickNavigationTabScrollOffset_, 0,
        GetQuickNavigationMaxTabScrollOffset(overlay));
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(overlay));
    const std::vector<RECT> itemRects =
        GetQuickNavigationItemRects(
            overlay, contentModel);

    const RECT searchRect = GetQuickNavigationSearchRect(overlay);
    DrawD2DRoundedRectangle(ctx.Get(),
        searchRect,
        static_cast<float>(QuickNavScale(12)) / 2.0f,
        ToD2DColor(t.searchBg), ToD2DColor(t.searchBorder));

    if (!searching)
    {
        RECT tabs = GetQuickNavigationTabsRect(overlay);
        const int tabsStart =
            GetQuickNavigationTabsStart(overlay);
        const int tabClipRight = tabs.right;
        // 左侧外扩一圈避免固定标签圆角 AA 被截断；右侧与搜索框右边界对齐。
        const int clipPad = QuickNavScale(8);
        ctx->PushAxisAlignedClip(
            ToD2DRect(MakeRect(
                tabsStart - clipPad,
                tabs.top, tabClipRight,
                tabs.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const size_t tabCount = collectionIndices.size() + 2;
        UpdateQuickNavTabWidths();
        const auto& tabWidths = quickNavTabWidths_;
        const int gap = QuickNavScale(8);
        const int sepGap = QuickNavScale(6);
        const int fixedWidth = (tabWidths.size() >= 2 ? tabWidths[0] + gap + tabWidths[1] : 0);
        const int scrollPad = sepGap + QuickNavScale(1) + gap; // separator + gap after
        auto calcTabPosX = [&](size_t tabIdx) -> int {
            if (tabIdx == 0) return tabsStart;
            if (tabIdx == 1) return tabsStart + tabWidths[0] + gap;
            int x = tabsStart + fixedWidth + scrollPad;
            for (size_t i = 2; i < tabIdx && i < tabWidths.size(); ++i)
                x += tabWidths[i] + gap;
            return x - quickNavigationTabScrollOffset_;
        };

        int dragTargetTab = -1;
        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1))
            dragTargetTab = GetQuickNavTabDragTarget(
                quickNavTabDragIndex_, quickNavTabDragDeltaX_);

        const int tabInsetY = QuickNavScale(3);
        auto drawTab = [&](size_t tab, int offsetX) {
            if (tab >= tabWidths.size()) return;
            int posX = calcTabPosX(tab) + offsetX;
            int tw = tabWidths[tab];
            // 按钮上下内缩，避免 AA 圆角被 tabs 裁剪边界截断，同时降低视觉高度。
            RECT tabRect = MakeRect(posX, tabs.top + tabInsetY, posX + tw, tabs.bottom - tabInsetY);
            if (tab >= 2)
            {
                int scrollStart =
                    tabsStart + fixedWidth +
                    scrollPad;
                if (tabRect.right <= scrollStart || tabRect.left >= tabClipRight) return;
            }
            else if (tab <= 1)
            {
                if (tabRect.right <= tabsStart ||
                    tabRect.left >=
                        tabsStart + fixedWidth +
                            sepGap)
                    return;
                tabRect.right = std::min<LONG>(
                    tabRect.right,
                    static_cast<LONG>(
                        tabsStart + fixedWidth +
                        sepGap));
            }
            if (!quickNavTabDragging_)
                registerHoverRegion(
                    tabRect,
                    QuickNavigationPointerTargetKind::Tab,
                    tab);
            const bool active = (tab == 0 && quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-1))
                || (tab == 1 && quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-2))
                || (tab > 1 && quickNavigationActiveWidgetIndex_ == collectionIndices[tab - 2]);
            bool hovered = false;
            if (!quickNavTabDragging_)
                hovered = PtInRect(&tabRect, lastMousePoint_) != FALSE;

            D2D1_COLOR_F fill, stroke;
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
            {
                fill = ToD2DColor(t.tabDragFill, 0.78f);
                stroke = ToD2DColor(t.tabDragStroke, 0.82f);
            }
            else
            {
                fill = active ? ToD2DColor(t.tabActiveFill, 0.82f)
                    : (hovered ? ToD2DColor(t.tabHoverFill, 0.72f)
                               : ToD2DColor(t.tabDefaultFill, 0.62f));
                stroke = active ? ToD2DColor(t.tabActiveStroke, 0.88f)
                                : ToD2DColor(t.tabDefaultStroke, 0.72f);
            }
            DrawD2DRoundedRectangle(ctx.Get(), tabRect,
                static_cast<float>(QuickNavScale(14)) / 2.0f, fill, stroke);

            std::wstring label = GetQuickNavTabLabel(tab);
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DTextEllipsis(ctx.Get(), label, textRect, quickNavTabTextFormat_.Get(),
                active ? ToD2DColor(RGB(245, 248, 252)) : ToD2DColor(t.tabText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        };

        // 分隔线：固定在 "映射" 标签右侧
        int sepX =
            tabsStart + fixedWidth + sepGap;
        RECT sepRect = MakeRect(sepX, tabs.top + QuickNavScale(8),
            sepX + QuickNavScale(1), tabs.bottom - QuickNavScale(8));
        DrawD2DSeparator(ctx.Get(), sepRect, ToD2DColor(t.tabSeparator));

        // Draw fixed tabs (0, 1) and dragged tab displacement
        for (size_t tab = 0; tab < tabCount && tab < 2; ++tab)
        {
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
                continue;
            int offsetX = 0;
            if (quickNavTabDragging_ && dragTargetTab >= 1)
            {
                size_t src = quickNavTabDragIndex_;
                int dst = dragTargetTab;
                int cur = static_cast<int>(tab);
                int shift = (quickNavTabDragIndex_ < tabWidths.size()
                    ? tabWidths[quickNavTabDragIndex_] + gap : tabWidths[0] + gap);
                if (cur > src && cur <= dst) offsetX = -shift;
                else if (cur < src && cur >= dst) offsetX = shift;
            }
            drawTab(tab, offsetX);
        }

        // Clip to scrollable area for remaining tabs
        int scrollLeft =
            tabsStart + fixedWidth + scrollPad;
        ctx->PopAxisAlignedClip();
        ctx->PushAxisAlignedClip(
            ToD2DRect(MakeRect(scrollLeft - clipPad, tabs.top, tabClipRight, tabs.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        for (size_t tab = 2; tab < tabCount; ++tab)
        {
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
                continue;
            int offsetX = 0;
            if (quickNavTabDragging_ && dragTargetTab >= 1)
            {
                size_t src = quickNavTabDragIndex_;
                int dst = dragTargetTab;
                int cur = static_cast<int>(tab);
                int shift = (quickNavTabDragIndex_ < tabWidths.size()
                    ? tabWidths[quickNavTabDragIndex_] + gap : tabWidths[0] + gap);
                if (cur > src && cur <= dst) offsetX = -shift;
                else if (cur < src && cur >= dst) offsetX = shift;
            }
            drawTab(tab, offsetX);
        }

        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1) &&
            quickNavTabDragIndex_ < tabWidths.size())
        {
            int dragTw = tabWidths[quickNavTabDragIndex_];
            int posX = calcTabPosX(quickNavTabDragIndex_) + quickNavTabDragDeltaX_;
            RECT tabRect = MakeRect(posX, tabs.top + tabInsetY, posX + dragTw, tabs.bottom - tabInsetY);
            tabRect.left = std::max(
                tabRect.left,
                static_cast<LONG>(
                    tabsStart));
            tabRect.right = std::min<LONG>(tabRect.right, static_cast<LONG>(tabClipRight));
            DrawD2DRoundedRectangle(ctx.Get(), tabRect,
                static_cast<float>(QuickNavScale(14)) / 2.0f,
                ToD2DColor(t.tabDragFloatFill, 0.82f),
                ToD2DColor(t.tabDragFloatStroke, 0.88f));

            std::wstring label = GetQuickNavTabLabel(quickNavTabDragIndex_);
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DTextEllipsis(ctx.Get(), label, textRect, quickNavTabTextFormat_.Get(),
                ToD2DColor(t.tabDragFloatText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            if (dragTargetTab >= 2 && static_cast<size_t>(dragTargetTab) <= collectionIndices.size() + 1)
            {
                int insertX = calcTabPosX(static_cast<size_t>(dragTargetTab));
                RECT ind = MakeRect(insertX - gap / 2, tabs.top + QuickNavScale(4),
                    insertX - gap / 2 + QuickNavScale(2), tabs.bottom - QuickNavScale(4));
                DrawD2DSeparator(ctx.Get(), ind, ToD2DColor(t.tabDragIndicator));
            }
        }

        ctx->PopAxisAlignedClip();

        const RECT modeButton =
            GetQuickNavigationViewModeButtonRect(
                overlay);
        if (!IsRectEmpty(&modeButton))
        {
            registerHoverRegion(
                modeButton,
                QuickNavigationPointerTargetKind::ViewMode);
            const bool hovered =
                PtInRect(
                    &modeButton,
                    lastMousePoint_) != FALSE;
            const D2D1_COLOR_F fill =
                hovered
                    ? ToD2DColor(
                        t.tabHoverFill, 0.82f)
                    : ToD2DColor(
                        t.tabActiveFill, 0.72f);
            const D2D1_COLOR_F stroke =
                hovered
                ? ToD2DColor(
                    t.tabActiveStroke, 0.92f)
                : ToD2DColor(
                    t.tabDefaultStroke, 0.76f);
            DrawD2DRoundedRectangle(
                ctx.Get(), modeButton,
                static_cast<float>(
                    QuickNavScale(14)) / 2.0f,
                fill, stroke);
            const std::wstring_view glyph =
                snowdesktop::
                    quick_navigation_rules::
                        QuickNavigationDesktopViewModeGlyph(
                            navigationSettings_.
                                desktopViewMode);
            DrawD2DText(
                ctx.Get(),
                std::wstring(glyph),
                modeButton,
                quickNavFluentTextFormat_
                ? quickNavFluentTextFormat_.Get()
                : (fluentIconTextFormat_
                    ? fluentIconTextFormat_.Get()
                    : quickNavTabTextFormat_.Get()),
                ToD2DColor(t.tabText));
        }

        // Tab strip overflow arrows: indicate and scroll hidden tabs.
        const int maxTabScroll =
            GetQuickNavigationMaxTabScrollOffset(
                overlay);
        if (maxTabScroll > 0)
        {
            const RECT rightBtn =
                GetQuickNavigationTabScrollButtonRect(
                    false);
            registerHoverRegion(
                rightBtn,
                QuickNavigationPointerTargetKind::
                    TabScrollRight);
            const bool rightHovered =
                PtInRect(
                    &rightBtn,
                    lastMousePoint_) != FALSE;
            DrawD2DRoundedRectangle(
                ctx.Get(), rightBtn,
                static_cast<float>(
                    QuickNavScale(8)),
                ToD2DColor(
                    t.tabHoverFill,
                    rightHovered ? 0.85f : 0.55f),
                ToD2DColor(
                    t.tabDefaultStroke,
                    rightHovered ? 0.9f : 0.6f));
            DrawD2DText(
                ctx.Get(), L"\u203A", rightBtn,
                quickNavTabTextFormat_.Get(),
                ToD2DColor(t.tabText));

            if (quickNavigationTabScrollOffset_ > 0)
            {
                const RECT leftBtn =
                    GetQuickNavigationTabScrollButtonRect(
                        true);
                registerHoverRegion(
                    leftBtn,
                    QuickNavigationPointerTargetKind::
                        TabScrollLeft);
                const bool leftHovered =
                    PtInRect(
                        &leftBtn,
                        lastMousePoint_) != FALSE;
                DrawD2DRoundedRectangle(
                    ctx.Get(), leftBtn,
                    static_cast<float>(
                        QuickNavScale(8)),
                    ToD2DColor(
                        t.tabHoverFill,
                        leftHovered ? 0.85f : 0.55f),
                    ToD2DColor(
                        t.tabDefaultStroke,
                        leftHovered ? 0.9f : 0.6f));
                DrawD2DText(
                    ctx.Get(), L"\u2039", leftBtn,
                    quickNavTabTextFormat_.Get(),
                    ToD2DColor(t.tabText));
            }
        }
    }

    RECT contentApp = GetQuickNavigationContentRect(overlay);
    ctx->PushAxisAlignedClip(ToD2DRect(contentApp), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (quickNavigationInitialJumpOpen_)
    {
        RECT titleRect = MakeRect(
            contentApp.left + QuickNavScale(8),
            contentApp.top,
            contentApp.right - QuickNavScale(96),
            contentApp.top + QuickNavScale(30));
        DrawD2DTextEllipsis(
            ctx.Get(),
            _LW("app.nav.initial_jump_title"),
            titleRect,
            quickNavTabTextFormat_.Get(),
            ToD2DColor(t.headerText),
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const RECT backRect =
            GetQuickNavigationInitialJumpBackRect(
                overlay);
        registerHoverRegion(
            backRect,
            QuickNavigationPointerTargetKind::InitialBack);
        const bool backHovered =
            PtInRect(&backRect,
                lastMousePoint_) != FALSE;
        if (backHovered)
            DrawD2DRoundedRectangle(
                ctx.Get(), backRect,
                static_cast<float>(
                    QuickNavScale(10)) / 2.0f,
                ToD2DColor(
                    t.tabHoverFill, 0.72f),
                ToD2DColor(
                    t.tabDefaultStroke, 0.72f));
        DrawD2DTextEllipsis(
            ctx.Get(),
            _LW("app.nav.initial_jump_back"),
            backRect,
            quickNavTabTextFormat_.Get(),
            ToD2DColor(t.tabText),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        std::array<bool, 27> available{};
        for (const auto& section :
            contentModel.sections)
        {
            if (section.label.size() != 1)
                continue;
            available[
                snowdesktop::
                    quick_navigation_rules::
                        InitialJumpBucketIndex(
                            section.label.front())] =
                                true;
        }
        for (size_t bucketIndex = 0;
            bucketIndex < available.size();
            ++bucketIndex)
        {
            const RECT cell =
                GetQuickNavigationInitialJumpCellRect(
                    overlay, bucketIndex);
            if (available[bucketIndex])
                registerHoverRegion(
                    cell,
                    QuickNavigationPointerTargetKind::InitialBucket,
                    bucketIndex);
            const bool hovered =
                PtInRect(
                    &cell,
                    lastMousePoint_) != FALSE;
            const bool selected =
                bucketIndex ==
                    quickNavigationInitialJumpSelection_;
            if (available[bucketIndex] &&
                (hovered || selected))
                DrawD2DRoundedRectangle(
                    ctx.Get(), cell,
                    static_cast<float>(
                        QuickNavScale(10)) / 2.0f,
                    ToD2DColor(
                        selected
                        ? t.tabActiveFill
                        : t.tabHoverFill,
                        0.78f),
                    ToD2DColor(
                        selected
                        ? t.tabActiveStroke
                        : t.tabDefaultStroke,
                        0.76f));
            const std::wstring label(
                1,
                snowdesktop::
                    quick_navigation_rules::
                        InitialJumpBucketAt(
                            bucketIndex));
            DrawD2DTextEllipsis(
                ctx.Get(), label, cell,
                quickNavTabTextFormat_.Get(),
                ToD2DColor(
                    available[bucketIndex]
                    ? t.tabText
                    : t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    else if (entries.empty() && quickNavigationAppResultIndices_.empty() &&
        (!searching || quickNavigationEverythingResults_.empty()))
    {
        RECT emptyRect = contentApp;
        emptyRect.top += QuickNavScale(28);
        if (searching && !everythingSearchAvailable_)
            DrawD2DTextEllipsis(ctx.Get(), GetQuickNavigationEverythingNoticeText(),
                emptyRect, quickNavItemTextFormat_.Get(), ToD2DColor(t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        else
            DrawD2DTextEllipsis(ctx.Get(),
                !searching
                ? (collectionIndices.empty() ? _LW("app.nav.empty_collection") : _LW("app.nav.empty_category"))
                : _LW("app.nav.no_results"),
                emptyRect, quickNavItemTextFormat_.Get(), ToD2DColor(t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    }
    else
    {
        if (searching)
        {
            const int headerH = QuickNavScale(28);
            RECT desktopHeader = MakeRect(contentApp.left + QuickNavScale(8),
                contentApp.top - quickNavigationScrollOffset_,
                contentApp.right - QuickNavScale(12),
                contentApp.top + headerH - quickNavigationScrollOffset_);
            std::wstring desktopLabel = _LFW("app.nav.desktop_results",
                std::to_wstring(entries.size()));
            DrawD2DTextEllipsis(ctx.Get(), desktopLabel, desktopHeader,
                quickNavTabTextFormat_.Get(), ToD2DColor(t.headerText),
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            RECT desktopSep = MakeRect(desktopHeader.left,
                desktopHeader.bottom - QuickNavScale(1),
                desktopHeader.right, desktopHeader.bottom);
            DrawD2DSeparator(ctx.Get(), desktopSep, ToD2DColor(t.headerSeparator));
        }
        else if (contentModel.IsSectioned())
        {
            for (size_t sectionIndex = 0;
                sectionIndex <
                    contentModel.sections.size();
                ++sectionIndex)
            {
                const auto& section =
                    contentModel.sections[
                        sectionIndex];
                const RECT header =
                    GetQuickNavigationSectionHeaderRect(
                        overlay, sectionIndex,
                        contentModel);
                if (header.bottom <= contentApp.top ||
                    header.top >= contentApp.bottom)
                    continue;
                const bool initialJumpHeader =
                    quickNavigationActiveWidgetIndex_ ==
                        static_cast<size_t>(-1) &&
                    navigationSettings_.desktopViewMode ==
                        QuickNavigationDesktopViewMode::
                            Initial;
                if (initialJumpHeader)
                    registerClippedHoverRegion(
                        header, contentApp,
                        QuickNavigationPointerTargetKind::SectionHeader,
                        sectionIndex);
                if (initialJumpHeader &&
                    PtInRect(
                        &header,
                        lastMousePoint_) != FALSE)
                    DrawD2DRoundedRectangle(
                        ctx.Get(), header,
                        static_cast<float>(
                            QuickNavScale(8)) / 2.0f,
                        ToD2DColor(
                            t.tabHoverFill, 0.58f),
                        ToD2DColor(
                            t.tabDefaultStroke,
                            0.62f));
                const std::wstring label = _LFW(
                    "app.nav.section_header",
                    section.label,
                    std::to_wstring(
                        section.entryCount));
                DrawD2DTextEllipsis(
                    ctx.Get(), label, header,
                    quickNavTabTextFormat_.Get(),
                    ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const RECT separator = MakeRect(
                    header.left,
                    header.bottom -
                        QuickNavScale(1),
                    header.right,
                    header.bottom);
                DrawD2DSeparator(
                    ctx.Get(), separator,
                    ToD2DColor(
                        t.headerSeparator));
            }
        }

        // 桌面项直接走 D2D（ctx 为 ID2D1DeviceContext，与桌面共享 d2dDevice_ 缓存）
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const RECT itemRectApp = itemRects[i];
            if (itemRectApp.bottom <= contentApp.top ||
                itemRectApp.top >= contentApp.bottom)
                continue;

            registerClippedHoverRegion(
                itemRectApp, contentApp,
                QuickNavigationPointerTargetKind::Item,
                i);

            const QuickNavigationEntry& entry = entries[i];
            const int state = (PtInRect(&itemRectApp, lastMousePoint_) != FALSE ||
                IsQuickNavigationKeyboardTarget(
                    QuickNavigationKeyboardTargetKind::Item, i)) ? 1 : 0;
            // 图标本体复用桌面绘制，标题由快捷导航自绘，避免桌面标题布局和字重互相影响。
            if (entry.kind == QuickNavigationEntry::Kind::DesktopItem &&
                entry.itemIndex < items_.size())
            {
                DesktopIcon icon(&items_[entry.itemIndex], nullptr, this);
                icon.Draw(ctx.Get(), itemRectApp, state, quickNavLightTheme_, false, true);
                DrawQuickNavItemText(ctx.Get(), itemRectApp, items_[entry.itemIndex].name,
                    false, quickNavLightTheme_);
            }
            else if (entry.kind == QuickNavigationEntry::Kind::FolderEntry &&
                entry.widgetIndex < widgets_.size() &&
                entry.folderEntryIndex < widgets_[entry.widgetIndex].folderEntries.size())
            {
                FolderEntry& folderEntry =
                    widgets_[entry.widgetIndex].folderEntries[entry.folderEntryIndex];
                FolderEntryIcon icon(&folderEntry, nullptr, this);
                icon.Draw(ctx.Get(), itemRectApp, state, quickNavLightTheme_, false, true);
                DrawQuickNavItemText(ctx.Get(), itemRectApp, folderEntry.name,
                    false, quickNavLightTheme_);
            }
        }

        if (searching)
        {
            const int columns = GetQuickNavigationColumnCount(overlay);
            const int desktopRows = entries.empty() ? 0 :
                (static_cast<int>(entries.size()) + columns - 1) / columns;
            const int headerH = QuickNavScale(28);
            const int gap = QuickNavScale(8);
            const int rowH = QuickNavScale(46);
            const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
                QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
            const int appHeaderTop = contentApp.top + headerH + gap
                + desktopGridH
                + gap - quickNavigationScrollOffset_;
            int everythingHeaderTop = appHeaderTop;

            if (!quickNavigationAppResultIndices_.empty())
            {
                const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
                RECT appHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    appHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    appHeaderTop + headerH);
                std::wstring appLabel = _LFW("app.nav.app_results",
                    std::to_wstring(quickNavigationAppResultIndices_.size()));
                DrawD2DTextEllipsis(ctx.Get(), appLabel, appHeader,
                    quickNavTabTextFormat_.Get(), ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                RECT appSep = MakeRect(appHeader.left, appHeader.bottom - QuickNavScale(1),
                    appHeader.right, appHeader.bottom);
                DrawD2DSeparator(ctx.Get(), appSep, ToD2DColor(t.headerSeparator));

                for (size_t i = 0; i < visibleAppCount; ++i)
                {
                    size_t appIndex = quickNavigationAppResultIndices_[i];
                    if (appIndex >= quickNavigationAppEntries_.size())
                        continue;

                    const int rowTop = appHeaderTop + headerH + gap + static_cast<int>(i) * rowH;
                    RECT rowRectApp = MakeRect(contentApp.left + QuickNavScale(8), rowTop,
                        contentApp.right - QuickNavScale(12), rowTop + rowH);
                    if (rowRectApp.bottom <= contentApp.top || rowRectApp.top >= contentApp.bottom)
                        continue;

                    registerClippedHoverRegion(
                        rowRectApp, contentApp,
                        QuickNavigationPointerTargetKind::App,
                        i);

                    if (PtInRect(&rowRectApp, lastMousePoint_) != FALSE ||
                        IsQuickNavigationKeyboardTarget(
                            QuickNavigationKeyboardTargetKind::App, i))
                        DrawD2DRoundedRectangle(ctx.Get(), rowRectApp,
                            static_cast<float>(QuickNavScale(10)) / 2.0f,
                            ToD2DColor(t.appRowHoverFill), ToD2DColor(t.appRowHoverStroke));

                    const QuickNavigationAppEntry& entry = quickNavigationAppEntries_[appIndex];
                    const int iconSz = QuickNavScale(28);
                    RECT iconRect = MakeRect(rowRectApp.left + QuickNavScale(12),
                        rowRectApp.top + (rowH - iconSz) / 2,
                        rowRectApp.left + QuickNavScale(12) + iconSz,
                        rowRectApp.top + (rowH + iconSz) / 2);
                    DrawQuickNavSysIcon(ctx.Get(), entry.systemIconIndex, iconRect);

                    const int textLeft = iconRect.right + QuickNavScale(10);
                    RECT nameRect = rowRectApp;
                    nameRect.left = textLeft;
                    nameRect.right -= QuickNavScale(12);
                    nameRect.top += QuickNavScale(5);
                    nameRect.bottom = nameRect.top + QuickNavScale(18);

                    RECT typeRect = rowRectApp;
                    typeRect.left = textLeft;
                    typeRect.right -= QuickNavScale(12);
                    typeRect.top += QuickNavScale(24);
                    typeRect.bottom -= QuickNavScale(5);

                    DrawD2DTextEllipsis(ctx.Get(), entry.name, nameRect,
                        quickNavItemTextFormat_.Get(), ToD2DColor(t.appNameText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DrawD2DTextEllipsis(ctx.Get(), _LW("app.nav.app_label"), typeRect,
                        quickNavPathTextFormat_.Get(), ToD2DColor(t.appTypeText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                int appRowsHeight = static_cast<int>(visibleAppCount) * rowH;
                if (HasQuickNavigationAppExpandButton())
                {
                    const int buttonTop = appHeaderTop + headerH + gap + appRowsHeight;
                    RECT buttonRectApp = MakeRect(contentApp.left + QuickNavScale(8), buttonTop,
                        contentApp.right - QuickNavScale(12), buttonTop + rowH);
                    if (buttonRectApp.bottom > contentApp.top && buttonRectApp.top < contentApp.bottom)
                    {
                        registerClippedHoverRegion(
                            buttonRectApp, contentApp,
                            QuickNavigationPointerTargetKind::ExpandApps);
                        const bool hovered = PtInRect(&buttonRectApp, lastMousePoint_) != FALSE ||
                            IsQuickNavigationKeyboardTarget(
                                QuickNavigationKeyboardTargetKind::ExpandApps, 0);
                        std::wstring expandLabel = _LFW("app.interact.expand_apps_fmt",
                            std::to_wstring(quickNavigationAppResultIndices_.size()));
                        DrawD2DTextEllipsis(ctx.Get(), expandLabel, buttonRectApp,
                            quickNavTabTextFormat_.Get(),
                            hovered ? ToD2DColor(t.expandHoverText) : ToD2DColor(t.expandDefaultText),
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                    appRowsHeight += rowH;
                }

                everythingHeaderTop = appHeaderTop + headerH + gap
                    + appRowsHeight
                    + gap;
            }

            if (!everythingSearchAvailable_)
            {
                RECT noticeHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    everythingHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    everythingHeaderTop + headerH);
                DrawD2DTextEllipsis(ctx.Get(), GetQuickNavigationEverythingNoticeText(), noticeHeader,
                    quickNavTabTextFormat_.Get(), ToD2DColor(t.emptyHeaderText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            else
            {
                RECT everythingHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    everythingHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    everythingHeaderTop + headerH);
                std::wstring everythingLabel = L"Everything  " +
                    std::to_wstring(quickNavigationEverythingResults_.size()) +
                    (quickNavigationEverythingHasMore_
                        ? _LW("app.interact.plus_items")
                        : _LW("app.interact.items_suffix"));
                DrawD2DTextEllipsis(ctx.Get(), everythingLabel, everythingHeader,
                    quickNavTabTextFormat_.Get(), ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                RECT evSep = MakeRect(everythingHeader.left,
                    everythingHeader.bottom - QuickNavScale(1),
                    everythingHeader.right, everythingHeader.bottom);
                DrawD2DSeparator(ctx.Get(), evSep, ToD2DColor(t.headerSeparator));

                for (size_t i = 0; i < quickNavigationEverythingResults_.size(); ++i)
                {
                    const int rowTop = everythingHeaderTop + headerH + gap
                        + static_cast<int>(i) * rowH;
                    RECT rowRectApp = MakeRect(contentApp.left + QuickNavScale(8), rowTop,
                        contentApp.right - QuickNavScale(12), rowTop + rowH);
                    if (rowRectApp.bottom <= contentApp.top || rowRectApp.top >= contentApp.bottom)
                        continue;

                    registerClippedHoverRegion(
                        rowRectApp, contentApp,
                        QuickNavigationPointerTargetKind::Everything,
                        i);

                    if (PtInRect(&rowRectApp, lastMousePoint_) != FALSE ||
                        IsQuickNavigationKeyboardTarget(
                            QuickNavigationKeyboardTargetKind::Everything, i))
                        DrawD2DRoundedRectangle(ctx.Get(), rowRectApp,
                            static_cast<float>(QuickNavScale(10)) / 2.0f,
                            ToD2DColor(t.appRowHoverFill), ToD2DColor(t.appRowHoverStroke));

                    const QuickNavigationEverythingEntry& entry = quickNavigationEverythingResults_[i];
                    const int iconSz = QuickNavScale(28);
                    RECT iconRect = MakeRect(rowRectApp.left + QuickNavScale(12),
                        rowRectApp.top + (rowH - iconSz) / 2,
                        rowRectApp.left + QuickNavScale(12) + iconSz,
                        rowRectApp.top + (rowH + iconSz) / 2);
                    DrawQuickNavSysIcon(ctx.Get(), entry.systemIconIndex, iconRect);

                    const int textLeft = iconRect.right + QuickNavScale(10);
                    RECT nameRect = rowRectApp;
                    nameRect.left = textLeft;
                    nameRect.right -= QuickNavScale(12);
                    nameRect.top += QuickNavScale(5);
                    nameRect.bottom = nameRect.top + QuickNavScale(18);

                    RECT pathRect = rowRectApp;
                    pathRect.left = textLeft;
                    pathRect.right -= QuickNavScale(12);
                    pathRect.top += QuickNavScale(24);
                    pathRect.bottom -= QuickNavScale(5);

                    DrawD2DTextEllipsis(ctx.Get(),
                        entry.name.empty() ? FileNameFromPath(entry.path) : entry.name,
                        nameRect, quickNavItemTextFormat_.Get(), ToD2DColor(t.appNameText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                    const std::wstring modifiedText = entry.modifiedText.empty()
                        ? QuickNavigationFormatModifiedTime(entry.dateModified)
                        : entry.modifiedText;
                    if (!modifiedText.empty())
                    {
                        const int textGap = QuickNavScale(8);
                        const int available = std::max<LONG>(1, pathRect.right - pathRect.left);
                        const int maxDateWidth = std::min(QuickNavScale(156), available);
                        const int minDateWidth = std::min(QuickNavScale(118), maxDateWidth);
                        const int dateWidth = std::clamp(
                            available / 3,
                            minDateWidth,
                            maxDateWidth);
                        RECT modifiedRect = pathRect;
                        modifiedRect.left = std::max<LONG>(modifiedRect.left,
                            modifiedRect.right - dateWidth);
                        pathRect.right = std::max<LONG>(pathRect.left,
                            modifiedRect.left - textGap);

                        DrawD2DTextEllipsis(ctx.Get(), modifiedText, modifiedRect,
                            quickNavPathTextFormat_.Get(), ToD2DColor(t.appTypeText),
                            DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                    DrawD2DTextEllipsis(ctx.Get(), entry.path, pathRect,
                        quickNavPathTextFormat_.Get(), ToD2DColor(t.appTypeText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                if (HasQuickNavigationEverythingLoadMoreButton())
                {
                    const int buttonTop = everythingHeaderTop + headerH + gap
                        + static_cast<int>(quickNavigationEverythingResults_.size()) * rowH;
                    RECT buttonRectApp = MakeRect(contentApp.left + QuickNavScale(8), buttonTop,
                        contentApp.right - QuickNavScale(12), buttonTop + rowH);
                    if (buttonRectApp.bottom > contentApp.top && buttonRectApp.top < contentApp.bottom)
                    {
                        registerClippedHoverRegion(
                            buttonRectApp, contentApp,
                            QuickNavigationPointerTargetKind::LoadMoreEverything);
                        const bool hovered = PtInRect(&buttonRectApp, lastMousePoint_) != FALSE ||
                            IsQuickNavigationKeyboardTarget(
                                QuickNavigationKeyboardTargetKind::LoadMoreEverything, 0);
                        DrawD2DTextEllipsis(ctx.Get(), _LW("app.nav.load_more_everything"), buttonRectApp,
                            quickNavTabTextFormat_.Get(),
                            hovered ? ToD2DColor(t.expandHoverText) : ToD2DColor(t.expandDefaultText),
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                }
            }
        }
    }
    ctx->PopAxisAlignedClip();

    RECT track{}, thumb{};
    int maxScroll = 0, contentHeight = 0;
    if (!quickNavigationInitialJumpOpen_ &&
        GetQuickNavigationScrollbarGeometry(overlay,
        track, thumb, maxScroll, contentHeight))
    {
        registerHoverRegion(
            thumb,
            QuickNavigationPointerTargetKind::Scrollbar);
        const int trackW = QuickNavScale(5);
        DrawD2DRoundedRectangle(ctx.Get(), track, static_cast<float>(trackW) / 2.0f,
            ToD2DColor(t.scrollTrack), ToD2DColor(t.scrollTrack));
        const COLORREF thumbColor = (quickNavScrollbarDragging_ || quickNavScrollbarHovered_)
            ? t.scrollThumbHover : t.scrollThumbDefault;
        DrawD2DRoundedRectangle(ctx.Get(), thumb, static_cast<float>(trackW) / 2.0f,
            ToD2DColor(thumbColor), ToD2DColor(thumbColor));
    }

    const RECT modeButton =
        GetQuickNavigationViewModeButtonRect(
            overlay);
    if (!IsRectEmpty(&modeButton) &&
        PtInRect(
            &modeButton,
            lastMousePoint_) != FALSE)
    {
        auto modeLabel = [](
            QuickNavigationDesktopViewMode mode) {
            return mode ==
                QuickNavigationDesktopViewMode::Source
                ? std::wstring(
                    _LW("app.nav.view_source"))
                : (mode ==
                    QuickNavigationDesktopViewMode::Initial
                    ? std::wstring(
                        _LW("app.nav.view_initial"))
                    : std::wstring(
                        _LW("app.nav.view_tile")));
        };
        const auto nextMode =
            snowdesktop::quick_navigation_rules::
                NextQuickNavigationDesktopViewMode(
                    navigationSettings_.
                        desktopViewMode);
        const std::wstring tooltip = _LFW(
            "app.nav.view_mode_tooltip",
            modeLabel(
                navigationSettings_.
                    desktopViewMode),
            modeLabel(nextMode));
        const int tooltipWidth =
            QuickNavScale(240);
        const int tooltipHeight =
            QuickNavScale(28);
        const int tooltipLeft =
            std::clamp(
                static_cast<int>(
                    modeButton.left),
                static_cast<int>(
                    overlay.left +
                    QuickNavScale(8)),
                std::max(
                    static_cast<int>(
                        overlay.left +
                        QuickNavScale(8)),
                    static_cast<int>(
                        overlay.right -
                        QuickNavScale(8) -
                        tooltipWidth)));
        const RECT tooltipRect = MakeRect(
            tooltipLeft,
            modeButton.bottom +
                QuickNavScale(4),
            tooltipLeft + tooltipWidth,
            modeButton.bottom +
                QuickNavScale(4) +
                tooltipHeight);
        DrawD2DRoundedRectangle(
            ctx.Get(), tooltipRect,
            static_cast<float>(
                QuickNavScale(10)) / 2.0f,
            t.popupBg,
            t.popupBorder);
        RECT tooltipTextRect = tooltipRect;
        tooltipTextRect.left +=
            QuickNavScale(8);
        tooltipTextRect.right -=
            QuickNavScale(8);
        DrawD2DTextEllipsis(
            ctx.Get(), tooltip,
            tooltipTextRect,
            quickNavPathTextFormat_.Get(),
            t.popupTitle,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (windowClipPushed)
        ctx->PopLayer();
    quickNavigationPointerTarget_ =
        HitTestQuickNavigationPointerTarget(
            lastMousePoint_);
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = quickNavDcompSurface_->EndDraw();
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"EndDraw", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    hr = dcompDevice_->Commit();
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"Paint Commit", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    quickNavCompositionRenderRecoveryPending_ = false;

    EndPaint(hwnd, &ps);
}
