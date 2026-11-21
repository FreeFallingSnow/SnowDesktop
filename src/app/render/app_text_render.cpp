#include "item_title_renderer.h"

#include "ui/render/app_font.h"
#include "ui/render/rgba_color.h"
#include "layout/item_title_layout.h"
#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/utils.h"
#include "layout/item_layout_rules.h"
#include "app/navigation/quick_navigation_theme.h"
#include "app/navigation/quick_navigation_helpers.h"

// DirectWrite item text rendering.

void DesktopApp::DrawItemSelectionBackground(ID2D1RenderTarget* context,
    RECT bounds, int state, bool lightTheme, float opacity, int titleLines)
{
    if (!context || (state != 1 && state != 2)) return;
    const bool selected = state == 2;
    const RECT frame = selected ? GetItemSelectionRect(bounds, true, titleLines) : bounds;
    const auto fill = selected
        ? (lightTheme ? D2D1::ColorF(.20f, .40f, .70f, .18f * opacity)
                      : D2D1::ColorF(.55f, .55f, .55f, .34f * opacity))
        : (lightTheme ? D2D1::ColorF(0, 0, 0, .06f * opacity)
                      : D2D1::ColorF(1, 1, 1, .08f * opacity));
    const auto border = selected
        ? (lightTheme ? D2D1::ColorF(.25f, .50f, .80f, .40f * opacity)
                      : D2D1::ColorF(.78f, .78f, .78f, .55f * opacity))
        : (lightTheme ? D2D1::ColorF(0, 0, 0, .12f * opacity)
                      : D2D1::ColorF(1, 1, 1, .20f * opacity));
    if (IsRectEmptyRect(frame)) return;
    constexpr float strokeWidth = 1.0f;
    DrawD2DRoundedRectangle(context, frame, 6.0f * GetItemLayoutScale(bounds), fill, border, strokeWidth);

}

void snowdesktop::ItemTitleRenderer::RecreateGridFormat(
    IDWriteFactory* factory, float fontSize, DWRITE_FONT_WEIGHT weight)
{
    if (!factory) return;
    gridLayouts_.clear();
    gridShadows_.clear();
    app_fonts::CreateTextFormat(factory, L"Segoe UI", weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &gridFormat_);
    if (!gridFormat_) return;
    gridFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    gridFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    gridFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    gridFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
        fontSize * 7.0f / 6.0f, fontSize * 5.0f / 6.0f);
}

void snowdesktop::ItemTitleRenderer::RecreateListFormat(
    IDWriteFactory* factory, float fontSize, DWRITE_FONT_WEIGHT weight)
{
    if (!factory) return;
    listLayouts_.clear();
    listShadows_.clear();
    listFormat_.Reset();
    app_fonts::CreateTextFormat(factory, L"Segoe UI", weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &listFormat_);
    if (!listFormat_) return;
    listFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    listFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    listFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    listFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
        fontSize * 7.0f / 6.0f, fontSize * 5.0f / 6.0f);
}

void snowdesktop::ItemTitleRenderer::InvalidateTitles()
{
    gridLayouts_.clear();
    listLayouts_.clear();
    gridShadows_.clear();
    listShadows_.clear();
}

void snowdesktop::ItemTitleRenderer::ReleaseDeviceResources()
{
    gridShadows_.clear();
    listShadows_.clear();
    effectContext_.Reset();
}

snowdesktop::ItemTitleRenderer::LayoutCacheUsage
snowdesktop::ItemTitleRenderer::GetLayoutCacheUsage(bool componentList) const
{
    LayoutCacheUsage usage;
    if (componentList)
    {
        usage.entries = listLayouts_.size();
        for (const auto& [key, layout] : listLayouts_)
        {
            (void)layout;
            usage.inputBytes += key.size() * sizeof(wchar_t);
        }
    }
    else
    {
        usage.entries = gridLayouts_.size();
        for (const auto& [key, entry] : gridLayouts_)
            usage.inputBytes += key.size() * sizeof(wchar_t) + entry.fullWidth.RetainedTextBytes();
    }
    return usage;
}

void snowdesktop::ItemTitleRenderer::VisitShadowBitmaps(bool componentList,
    const std::function<void(ID2D1Bitmap*)>& visit) const
{
    const auto& shadows = componentList ? listShadows_ : gridShadows_;
    for (const auto& [key, bitmap] : shadows)
    {
        (void)key;
        visit(bitmap.Get());
    }
}

void snowdesktop::ItemTitleRenderer::DrawStyledLayout(ID2D1RenderTarget* context,
    D2DDrawing& drawing, ID2D1Device* device,
    IDWriteTextLayout* layout, const std::wstring& shadowKey,
    D2D1_POINT_2F origin, D2D1_SIZE_F layoutSize,
    float layoutScale, float opacity, bool lightTheme,
    bool lightPanelShadow, bool componentList)
{
    if (!context || !layout || shadowKey.empty()) return;
    auto textBrush = drawing.GetSolidBrush(context,
        lightTheme ? D2D1::ColorF(0.10f, 0.12f, 0.16f, opacity)
                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity));

    const float tw = std::max(1.0f, layoutSize.width);
    const float th = std::max(1.0f, layoutSize.height);
    const float shadowScale = std::max(0.5f, layoutScale);
    // The selected panel style keeps one moderate soft shadow plus a close
    // offset shadow. Desktop labels retain their stronger, repeated soft layer.
    ComPtr<ID2D1DeviceContext> deviceContext;
    const bool supportsEffects =
        SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&deviceContext))) && deviceContext;

    if (supportsEffects && !effectContext_ && device)
    {
        device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &effectContext_);
        if (effectContext_)
        {
            effectContext_->SetDpi(96.0f, 96.0f);
            effectContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        }
    }

    // Cache by layout AND shadow style: an identical title can appear on both
    // the desktop and a glass panel during the same frame.
    if (supportsEffects && effectContext_ && !lightTheme)
    {
        auto& shadowCache = componentList
            ? listShadows_
            : gridShadows_;
        const std::wstring cacheKey = lightPanelShadow
            ? L"glass-panel\x1f" + shadowKey : shadowKey;
        auto shadowIt = shadowCache.find(cacheKey);
        if (shadowIt == shadowCache.end())
        {
            const UINT shadowPadding =
                static_cast<UINT>(std::max(1.0f, std::ceil(6.0f * shadowScale)));
            const UINT shadowWidth = static_cast<UINT>(
                std::ceil(tw + shadowPadding * 2.0f + shadowScale));
            const UINT shadowHeight = static_cast<UINT>(
                std::ceil(th + shadowPadding * 2.0f + shadowScale));

            ComPtr<ID2D1CommandList> shadowMask;
            ComPtr<ID2D1Bitmap1> shadowBitmap;
            D2D1_BITMAP_PROPERTIES1 shadowBitmapProperties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(effectContext_->CreateCommandList(&shadowMask)) &&
                shadowMask &&
                SUCCEEDED(effectContext_->CreateBitmap(
                    D2D1::SizeU(shadowWidth, shadowHeight), nullptr, 0,
                    &shadowBitmapProperties, &shadowBitmap)) &&
                shadowBitmap)
            {
                effectContext_->SetTarget(shadowMask.Get());
                effectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
                effectContext_->BeginDraw();

                ComPtr<ID2D1SolidColorBrush> maskBrush;
                if (SUCCEEDED(effectContext_->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &maskBrush)) && maskBrush)
                {
                    effectContext_->DrawTextLayout(
                        D2D1::Point2F(
                            static_cast<float>(shadowPadding),
                            static_cast<float>(shadowPadding)),
                        layout, maskBrush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }

                HRESULT maskHr = effectContext_->EndDraw();
                effectContext_->SetTarget(nullptr);
                if (SUCCEEDED(maskHr) && SUCCEEDED(shadowMask->Close()))
                {
                    ComPtr<ID2D1Effect> softShadow;
                    ComPtr<ID2D1Effect> offsetShadow;
                    ComPtr<ID2D1Effect> offsetTransform;
                    if (SUCCEEDED(effectContext_->CreateEffect(
                            CLSID_D2D1Shadow, &softShadow)) && softShadow &&
                        SUCCEEDED(effectContext_->CreateEffect(
                            CLSID_D2D1Shadow, &offsetShadow)) && offsetShadow &&
                        SUCCEEDED(effectContext_->CreateEffect(
                            CLSID_D2D12DAffineTransform, &offsetTransform)) &&
                        offsetTransform)
                    {
                        if (softShadow)
                        {
                            softShadow->SetInput(0, shadowMask.Get());
                            softShadow->SetValue(
                                D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                                (lightPanelShadow ? 1.2f : 1.5f) * shadowScale);
                            softShadow->SetValue(
                                D2D1_SHADOW_PROP_COLOR,
                                D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f,
                                    lightPanelShadow ? 0.65f : 0.95f });
                            softShadow->SetValue(
                                D2D1_SHADOW_PROP_OPTIMIZATION,
                                D2D1_SHADOW_OPTIMIZATION_QUALITY);
                        }

                        offsetShadow->SetInput(0, shadowMask.Get());
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                            0.5f * shadowScale);
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_COLOR,
                            D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f,
                                lightPanelShadow ? 0.80f : 1.0f });
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_OPTIMIZATION,
                            D2D1_SHADOW_OPTIMIZATION_QUALITY);

                        offsetTransform->SetInputEffect(0, offsetShadow.Get());
                        offsetTransform->SetValue(
                            D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                            D2D1::Matrix3x2F::Translation(
                                (lightPanelShadow ? 0.75f : 1.0f) * shadowScale,
                                (lightPanelShadow ? 0.75f : 1.0f) * shadowScale));
                        offsetTransform->SetValue(
                            D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                            D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);

                        effectContext_->SetTarget(shadowBitmap.Get());
                        effectContext_->BeginDraw();
                        effectContext_->Clear(
                            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                        if (softShadow)
                        {
                            effectContext_->DrawImage(softShadow.Get());
                            if (!lightPanelShadow)
                                effectContext_->DrawImage(softShadow.Get());
                        }
                        effectContext_->DrawImage(offsetTransform.Get());
                        HRESULT shadowHr = effectContext_->EndDraw();
                        effectContext_->SetTarget(nullptr);
                        if (SUCCEEDED(shadowHr))
                        {
                            shadowIt = shadowCache.emplace(
                                cacheKey, std::move(shadowBitmap)).first;
                        }
                    }
                }
            }
        }

        if (shadowIt != shadowCache.end() && shadowIt->second)
        {
            const float shadowPadding =
                std::max(1.0f, std::ceil(6.0f * shadowScale));
            const D2D1_SIZE_F shadowSize = shadowIt->second->GetSize();
            context->DrawBitmap(
                shadowIt->second.Get(),
                D2D1::RectF(
                    origin.x - shadowPadding,
                    origin.y - shadowPadding,
                    origin.x - shadowPadding + shadowSize.width,
                    origin.y - shadowPadding + shadowSize.height),
                opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    else
    {
        auto shadowBrush = drawing.GetSolidBrush(context, lightTheme
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f * opacity)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.80f * opacity));
        if (shadowBrush)
        {
            context->DrawTextLayout(
                D2D1::Point2F(
                    origin.x + (lightPanelShadow ? 0.75f : 1.0f) * layoutScale,
                    origin.y + (lightPanelShadow ? 0.75f : 1.0f) * layoutScale),
                layout, shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (!lightPanelShadow)
                context->DrawTextLayout(
                    D2D1::Point2F(origin.x, origin.y + layoutScale),
                    layout, shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (textBrush)
        context->DrawTextLayout(origin, layout, textBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DesktopApp::DrawItemText(ID2D1RenderTarget* context, RECT bounds,
    const std::wstring& text, bool selected, float opacity, bool lightTheme,
    bool componentPanel, int titleLines)
{
    titleLines = titleLines > 0 ? std::clamp(titleLines, 1, 2) : desktopTitleLines_;
    const int componentTitleLines = componentPanel ? titleLines : 0;
    const auto metrics = GetItemVisualMetrics(bounds, componentTitleLines);
    itemTitleRenderer_.DrawGrid(context, drawing_, d2dDevice_.Get(), dwriteFactory_.Get(),
        GetItemTextRect(bounds, selected, componentTitleLines), text,
        {metrics.fontSize, metrics.fontScale, metrics.titleHeight, titleLines, opacity,
            selected, lightTheme, componentPanel, titleEllipsis_},
        itemFontWeight_, componentPanel && CurrentPersonalization().glassEnabled);
}

void DesktopApp::DrawComponentListTitle(ID2D1DeviceContext* context, RECT textRect,
    const std::wstring& text, float scale, float fontSize, bool lightTheme,
    const snowdesktop::ItemTitleRenderer::ScrollOffset& scrollOffset)
{
    itemTitleRenderer_.DrawList(context, drawing_, d2dDevice_.Get(), dwriteFactory_.Get(),
        textRect, text, scale, fontSize, itemFontWeight_, lightTheme,
        CurrentPersonalization().glassEnabled, scrollOffset);
}

void snowdesktop::ItemTitleRenderer::DrawGrid(ID2D1RenderTarget* context, D2DDrawing& drawing,
    ID2D1Device* device, IDWriteFactory* factory, RECT textRect,
    const std::wstring& text, const GridStyle& style, DWRITE_FONT_WEIGHT weight, bool glassEnabled)
{
    if (!context || !factory || !gridFormat_ || text.empty()) return;

    const bool selected = style.selected;
    const bool componentPanel = style.componentPanel;
    const bool lightTheme = style.lightTheme;
    const int titleLines = style.lines;
    const float fontSize = style.fontSize;
    const float fontScale = style.fontScale;
    const float lineHeight = fontSize * 7.0f / 6.0f;
    const float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
    float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
    if (!selected)
    {
        th = static_cast<float>(snowdesktop::item_layout_rules::TextHeightForLineCount(lineHeight, titleLines));
        textRect.bottom = textRect.top + static_cast<LONG>(th);
    }
    const auto fontWeight = static_cast<DWRITE_FONT_WEIGHT>(
        font_weight_rules::RenderedWeight(weight, lightTheme));
    auto createConfiguredLayout =
        [&](const std::wstring& layoutText,
            float maxWidth,
            float maxHeight,
            ComPtr<IDWriteTextLayout>& result) {
        result.Reset();
        if (FAILED(
                factory->CreateTextLayout(
                    layoutText.c_str(),
                    static_cast<UINT32>(
                        layoutText.size()),
                    gridFormat_.Get(),
                    maxWidth, maxHeight,
                    &result)) ||
            !result)
        {
            return false;
        }

        const DWRITE_TEXT_RANGE range{
            0,
            static_cast<UINT32>(
                layoutText.size())
        };
        result->SetFontSize(
            fontSize,
            range);
        snowdesktop::app_fonts::SetWeight(result.Get(), fontWeight, range);
        result->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
        result->SetLineSpacing(
            DWRITE_LINE_SPACING_METHOD_UNIFORM,
            lineHeight,
            fontSize * 5.0f / 6.0f);
        return true;
    };
    const int scaleKey = static_cast<int>(std::round(fontScale * 1000.0f));
    std::wstring layoutKey = L"grid\x1f" + text + L"\x1f" +
        std::to_wstring(textRect.right - textRect.left) + L"x" +
        std::to_wstring(textRect.bottom - textRect.top) + L"@" +
        std::to_wstring(scaleKey) + L"@" +
        std::to_wstring(lightTheme ? 1 : 0) + L"@" +
        std::to_wstring(selected ? 1 : 0) + L"@" + std::to_wstring(titleLines) + L"@" +
        std::to_wstring(style.ellipsis ? 1 : 0);
    auto layoutIt = gridLayouts_.find(layoutKey);
    if (layoutIt == gridLayouts_.end())
    {
        snowdesktop::ItemTextLayoutCacheEntry cached;
        std::wstring visibleText = text;

        float layoutHeight = selected ? th : lineHeight * static_cast<float>(titleLines);
        if (selected)
        {
            const int measurementHeight =
                snowdesktop::item_layout_rules::
                    TextHeightForLineCount(
                        lineHeight,
                        text.size() + 1);
            ComPtr<IDWriteTextLayout>
                fullMeasureLayout;
            if (createConfiguredLayout(
                    text, tw,
                    static_cast<float>(
                        measurementHeight),
                    fullMeasureLayout))
            {
                DWRITE_TEXT_METRICS
                    fullMetrics{};
                if (SUCCEEDED(
                        fullMeasureLayout->
                            GetMetrics(
                                &fullMetrics)))
                {
                    layoutHeight = std::max(
                        layoutHeight,
                        static_cast<float>(
                            snowdesktop::
                                item_layout_rules::
                                    TextHeightForLineCount(
                                        lineHeight,
                                        fullMetrics.
                                            lineCount)));
                }
            }
        }

        ComPtr<IDWriteTextLayout> layout;
        if (!createConfiguredLayout(
                visibleText, tw,
                layoutHeight, layout))
            return;

        if (!selected)
        {
            ComPtr<IDWriteTextFormat> ellipsisFormat;
            snowdesktop::app_fonts::CreateTextFormat(factory, L"Segoe UI", fontWeight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &ellipsisFormat);
            snowdesktop::TrimItemTitle(factory, layout.Get(), ellipsisFormat.Get(), titleLines, lineHeight, style.ellipsis);
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        bool isSingleLine = (metrics.lineCount == 1);
        if (!isSingleLine)
            isSingleLine = cached.fullWidth.FitsSingleLine(factory,
                gridFormat_.Get(), text, fontSize, fontWeight, tw).fitsSingleLine;
        if (isSingleLine)
        {
            // Use the same exact centering band in both states. The expanded
            // clipping height includes rounding and anti-aliasing clearance;
            // centering there and compensating the origin moves short labels.
            if (selected && metrics.lineCount == 1)
                layout->SetMaxHeight(lineHeight * static_cast<float>(titleLines));
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        cached.layout = std::move(layout);
        layoutIt = gridLayouts_.emplace(std::move(layoutKey), std::move(cached)).first;
    }

    float ty = static_cast<float>(textRect.top);
    if (componentPanel && !selected && titleLines == 1)
        ty += std::max(0.0f, (static_cast<float>(style.titleHeight) - th) / 2.0f);
    DWRITE_TEXT_METRICS metrics{};
    layoutIt->second.layout->GetMetrics(&metrics);
    if (selected)
    {
        th = std::max(
            th,
            static_cast<float>(
                snowdesktop::
                    item_layout_rules::
                        TextHeightForLineCount(
                            lineHeight,
                            metrics.lineCount)));
        textRect.bottom = textRect.top +
            static_cast<LONG>(std::ceil(th));
    }
    bool isSingleLine = (metrics.lineCount == 1);
    // The full-width measurement only positions an expanded selected title.
    // Collapsed titles already have their cached layout and do not use it.
    if (selected && !isSingleLine)
        isSingleLine = layoutIt->second.fullWidth.FitsSingleLine(factory,
            gridFormat_.Get(), text, fontSize, fontWeight, tw).fitsSingleLine;
    if (isSingleLine && selected)
    {
        if (metrics.lineCount == 1)
        {
            if (componentPanel && titleLines == 1)
            {
                const float collapsedClipHeight = static_cast<float>(
                    snowdesktop::item_layout_rules::TextHeightForLineCount(lineHeight, titleLines));
                ty += std::max(0.0f,
                    (static_cast<float>(style.titleHeight) - collapsedClipHeight) * 0.5f);
            }
        }
        else
        {
            // Preserve the existing wide-measurement fallback for titles
            // that contain explicit newlines or barely exceed one line.
            ty = textRect.top + (static_cast<float>(style.titleHeight) - th) * 0.5f;
        }
    }

    DrawStyledLayout(
        context, drawing, device, layoutIt->second.layout.Get(), layoutIt->first,
        D2D1::Point2F(static_cast<float>(textRect.left), ty),
        D2D1::SizeF(tw, th), fontScale, style.opacity, lightTheme, !lightTheme && componentPanel && glassEnabled, false);
}

void snowdesktop::ItemTitleRenderer::DrawList(ID2D1DeviceContext* context, D2DDrawing& drawing,
    ID2D1Device* device, IDWriteFactory* factory, RECT textRect, const std::wstring& title,
    float layoutScale, float fontSize, DWRITE_FONT_WEIGHT weight, bool lightTheme,
    bool glassEnabled, const ScrollOffset& scrollOffset)
{
    if (!context || !factory || !listFormat_ || title.empty() || textRect.right <= textRect.left)
        return;

    const float width = static_cast<float>(textRect.right - textRect.left);
    const float height = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
    const auto fontWeight = static_cast<DWRITE_FONT_WEIGHT>(
        font_weight_rules::RenderedWeight(weight, lightTheme));
    const bool lightPanelShadow = !lightTheme && glassEnabled;
    const std::wstring layoutKey = L"list\x1f" + title + L"\x1f" +
        std::to_wstring(width) + L"x" + std::to_wstring(height) + L"@" +
        std::to_wstring(layoutScale) + L"@" + std::to_wstring(fontSize) + L"@" + std::to_wstring(fontWeight);
    // Both presentations use immutable layouts with the same explicit font.
    const auto getLayout = [&](bool scrolling) -> ComPtr<IDWriteTextLayout> {
        const std::wstring key = scrolling ? layoutKey + L"\x1fscroll" : layoutKey;
        if (const auto found = listLayouts_.find(key); found != listLayouts_.end()) return found->second;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()),
                listFormat_.Get(), width, height, &layout)) || !layout) return {};
        const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(title.size())};
        layout->SetFontSize(fontSize, range);
        snowdesktop::app_fonts::SetWeight(layout.Get(), fontWeight, range);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
            fontSize * 7.0f / 6.0f, fontSize * 5.0f / 6.0f);
        DWRITE_TRIMMING trimming{};
        if (scrolling)
        {
            layout->SetTrimming(&trimming, nullptr);
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(layout->GetMetrics(&metrics))) return {};
            layout->SetMaxWidth(std::max(width, std::ceil(metrics.widthIncludingTrailingWhitespace)));
        }
        else
        {
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            ComPtr<IDWriteInlineObject> ellipsis;
            ComPtr<IDWriteTextFormat> format;
            snowdesktop::app_fonts::CreateTextFormat(factory, L"Segoe UI", fontWeight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &format);
            if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(
                    format ? format.Get() : listFormat_.Get(), &ellipsis)) && ellipsis)
                layout->SetTrimming(&trimming, ellipsis.Get());
        }
        listLayouts_.emplace(key, layout);
        return layout;
    };
    if (scrollOffset)
    {
        if (const auto layout = getLayout(true))
        {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)))
            {
                const float naturalWidth = metrics.widthIncludingTrailingWhitespace;
                if (const auto offset = scrollOffset(naturalWidth, width))
                {
                    // Keep the caller's viewport clip; only this title moves.
                    context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(textRect.left), static_cast<float>(textRect.top),
                        static_cast<float>(textRect.right), static_cast<float>(textRect.bottom)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    const std::wstring shadowKey = layoutKey + L"\x1fscroll";
                    const float cycle = naturalWidth + std::max(1.0f, 24.0f * layoutScale);
                    for (const float shift : {0.0f, cycle})
                        DrawStyledLayout(context, drawing, device, layout.Get(), shadowKey,
                            D2D1::Point2F(textRect.left - *offset + shift, static_cast<float>(textRect.top)),
                            D2D1::SizeF(layout->GetMaxWidth(), height), layoutScale, 1.0f,
                            lightTheme, lightPanelShadow, true);
                    context->PopAxisAlignedClip();
                    return;
                }
            }
        }
    }
    if (const auto layout = getLayout(false))
        DrawStyledLayout(context, drawing, device, layout.Get(), layoutKey,
            D2D1::Point2F(static_cast<float>(textRect.left), static_cast<float>(textRect.top)),
            D2D1::SizeF(width, height), layoutScale, 1.0f, lightTheme, lightPanelShadow, true);
}

void DesktopApp::DrawQuickNavItemText(ID2D1RenderTarget* ctx, RECT bounds,
    const std::wstring& text, bool selected, bool lightTheme)
{
    if (!ctx || !dwriteFactory_ || !quickNavigation_->itemTextFormat || text.empty())
        return;

    const float fontSize = quickNavigation_->itemTextFormat->GetFontSize();
    const float lineSpacing = QuickNavigationItemTextLineSpacing(fontSize);
    const float baseline = std::max(1.0f, std::floor(fontSize * 0.84f));
    const int textHeight = QuickNavigationItemTextHeight(fontSize, navigationSettings_.layout.labelLines);
    RECT iconRect = GetQuickNavItemIconRect(bounds);
    const int horizontalPad = QuickNavScale(4);
    const int topGap = std::max(1, QuickNavScale(10));
    RECT textRect = QuickNavigationItemTextRect(bounds, iconRect, horizontalPad, topGap, textHeight);
    if (IsRectEmptyRect(textRect))
        return;

    const float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
    const float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(),
        static_cast<UINT32>(text.size()), quickNavigation_->itemTextFormat.Get(),
        tw, th, &layout)) || !layout)
        return;

    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineSpacing, baseline);
    DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    ComPtr<IDWriteInlineObject> trimmingSign;
    if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(
        quickNavigation_->itemTextFormat.Get(), &trimmingSign)) && trimmingSign)
        layout->SetTrimming(&trimming, trimmingSign.Get());

    // Match desktop titles: a short label is centered in the reserved title
    // band while wrapped labels retain their first-line position.
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.lineCount == 1)
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const QuickNavTheme theme = ResolveQuickNavTheme(lightTheme, navigationSettings_);
    const auto textColor = selected ? theme.selectedText : theme.itemText;
    auto textBrush = drawing_.GetSolidBrush(ctx, ToD2DColor(textColor));
    if (!textBrush)
        return;

    const D2D1_POINT_2F origin = D2D1::Point2F(
        static_cast<float>(textRect.left),
        static_cast<float>(textRect.top));
    if (!lightTheme)
    {
        if (auto shadowBrush = drawing_.GetSolidBrush(ctx,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.38f * textColor.alpha)))
        {
            ctx->DrawTextLayout(
                D2D1::Point2F(origin.x + 1.0f, origin.y + 1.0f),
                layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    ctx->DrawTextLayout(origin, layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DesktopApp::DrawD2DText(ID2D1RenderTarget* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
    DWRITE_WORD_WRAPPING wordWrapping)
{
    drawing_.DrawBasicText(ctx, dwriteFactory_.Get(), text, rect, format, color, wordWrapping);
}

void DesktopApp::DrawD2DTextEllipsis(ID2D1RenderTarget* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
    DWRITE_TEXT_ALIGNMENT hAlign, DWRITE_PARAGRAPH_ALIGNMENT vAlign, bool ellipsis,
    IDWriteTextLayout* cachedLayout)
{
    drawing_.DrawTextEllipsis(ctx, dwriteFactory_.Get(), text, rect, format, color,
        hAlign, vAlign, ellipsis, cachedLayout);
}
