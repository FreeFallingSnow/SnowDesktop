#include "app.h"
#include "quick_navigation_theme.h"

// DirectWrite item text rendering.

void DesktopApp::DrawStyledItemTextLayout(ID2D1RenderTarget* context,
    IDWriteTextLayout* layout, const std::wstring& shadowKey,
    D2D1_POINT_2F origin, D2D1_SIZE_F layoutSize,
    float layoutScale, float opacity, bool lightTheme)
{
    if (!context || !layout || shadowKey.empty()) return;
    if (context != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = context;
    }

    auto getBrush = [&](const D2D1_COLOR_F& color) -> ID2D1SolidColorBrush* {
        const std::uint64_t key = D2DColorBrushKey(color);
        auto it = brushCache_.find(key);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(context->CreateSolidColorBrush(color, &brush)) || !brush)
                return nullptr;
            it = brushCache_.emplace(key, std::move(brush)).first;
        }
        return it->second.Get();
    };
    ID2D1SolidColorBrush* textBrush =
        getBrush(lightTheme
            ? D2D1::ColorF(0.10f, 0.12f, 0.16f, opacity)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity));

    const float tw = std::max(1.0f, layoutSize.width);
    const float th = std::max(1.0f, layoutSize.height);
    const float shadowScale = std::max(0.5f, layoutScale);
    ComPtr<ID2D1DeviceContext> deviceContext;
    const bool supportsEffects =
        SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&deviceContext))) && deviceContext;

    if (supportsEffects && !itemTextEffectContext_ && d2dDevice_)
    {
        d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &itemTextEffectContext_);
        if (itemTextEffectContext_)
        {
            itemTextEffectContext_->SetDpi(96.0f, 96.0f);
            itemTextEffectContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        }
    }

    // Render both shadow layers through Direct2D's continuous Gaussian shadow
    // effect. Cache the result per layout so normal desktop repaints only need
    // one bitmap draw per label.
    if (supportsEffects && itemTextEffectContext_ && !lightTheme)
    {
        auto shadowIt = itemTextShadowCache_.find(shadowKey);
        if (shadowIt == itemTextShadowCache_.end())
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
            if (SUCCEEDED(itemTextEffectContext_->CreateCommandList(&shadowMask)) &&
                shadowMask &&
                SUCCEEDED(itemTextEffectContext_->CreateBitmap(
                    D2D1::SizeU(shadowWidth, shadowHeight), nullptr, 0,
                    &shadowBitmapProperties, &shadowBitmap)) &&
                shadowBitmap)
            {
                itemTextEffectContext_->SetTarget(shadowMask.Get());
                itemTextEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
                itemTextEffectContext_->BeginDraw();

                ComPtr<ID2D1SolidColorBrush> maskBrush;
                if (SUCCEEDED(itemTextEffectContext_->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &maskBrush)) && maskBrush)
                {
                    itemTextEffectContext_->DrawTextLayout(
                        D2D1::Point2F(
                            static_cast<float>(shadowPadding),
                            static_cast<float>(shadowPadding)),
                        layout, maskBrush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }

                HRESULT maskHr = itemTextEffectContext_->EndDraw();
                itemTextEffectContext_->SetTarget(nullptr);
                if (SUCCEEDED(maskHr) && SUCCEEDED(shadowMask->Close()))
                {
                    ComPtr<ID2D1Effect> softShadow;
                    ComPtr<ID2D1Effect> offsetShadow;
                    ComPtr<ID2D1Effect> offsetTransform;
                    if (SUCCEEDED(itemTextEffectContext_->CreateEffect(
                        CLSID_D2D1Shadow, &softShadow)) && softShadow &&
                        SUCCEEDED(itemTextEffectContext_->CreateEffect(
                            CLSID_D2D1Shadow, &offsetShadow)) && offsetShadow &&
                        SUCCEEDED(itemTextEffectContext_->CreateEffect(
                            CLSID_D2D12DAffineTransform, &offsetTransform)) &&
                        offsetTransform)
                    {
                        softShadow->SetInput(0, shadowMask.Get());
                        softShadow->SetValue(
                            D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                            1.5f * shadowScale);
                        softShadow->SetValue(
                            D2D1_SHADOW_PROP_COLOR,
                            D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f, 0.95f });
                        softShadow->SetValue(
                            D2D1_SHADOW_PROP_OPTIMIZATION,
                            D2D1_SHADOW_OPTIMIZATION_QUALITY);

                        offsetShadow->SetInput(0, shadowMask.Get());
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                            0.5f * shadowScale);
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_COLOR,
                            D2D1_VECTOR_4F{ 0.0f, 0.0f, 0.0f, 1.0f });
                        offsetShadow->SetValue(
                            D2D1_SHADOW_PROP_OPTIMIZATION,
                            D2D1_SHADOW_OPTIMIZATION_QUALITY);

                        offsetTransform->SetInputEffect(0, offsetShadow.Get());
                        offsetTransform->SetValue(
                            D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                            D2D1::Matrix3x2F::Translation(shadowScale, shadowScale));
                        offsetTransform->SetValue(
                            D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                            D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);

                        itemTextEffectContext_->SetTarget(shadowBitmap.Get());
                        itemTextEffectContext_->BeginDraw();
                        itemTextEffectContext_->Clear(
                            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                        itemTextEffectContext_->DrawImage(softShadow.Get());
                        itemTextEffectContext_->DrawImage(softShadow.Get());
                        itemTextEffectContext_->DrawImage(offsetTransform.Get());
                        HRESULT shadowHr = itemTextEffectContext_->EndDraw();
                        itemTextEffectContext_->SetTarget(nullptr);
                        if (SUCCEEDED(shadowHr))
                        {
                            shadowIt = itemTextShadowCache_.emplace(
                                shadowKey, std::move(shadowBitmap)).first;
                        }
                    }
                }
            }
        }

        if (shadowIt != itemTextShadowCache_.end() && shadowIt->second)
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
        ID2D1SolidColorBrush* shadowBrush =
            getBrush(lightTheme
                ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f * opacity)
                : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.80f * opacity));
        if (shadowBrush)
        {
            context->DrawTextLayout(
                D2D1::Point2F(origin.x + layoutScale, origin.y + layoutScale),
                layout, shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            context->DrawTextLayout(
                D2D1::Point2F(origin.x, origin.y + layoutScale),
                layout, shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (textBrush)
        context->DrawTextLayout(origin, layout, textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DesktopApp::DrawItemText(ID2D1RenderTarget* context, RECT bounds,
    const std::wstring& text, bool selected, float opacity, bool lightTheme)
{
    if (!dwriteFactory_ || !itemTextFormat_ || text.empty()) return;

    RECT textRect = GetItemTextRect(bounds, selected);
    float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
    float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));

    const float layoutScale = GetItemLayoutScale(bounds);
    const int scaleKey = static_cast<int>(std::round(layoutScale * 1000.0f));
    std::wstring layoutKey = L"grid\x1f" + text + L"\x1f" +
        std::to_wstring(textRect.right - textRect.left) + L"x" +
        std::to_wstring(textRect.bottom - textRect.top) + L"@" +
        std::to_wstring(scaleKey) + L"@" +
        std::to_wstring(lightTheme ? 1 : 0) + L"@" +
        std::to_wstring(selected ? 1 : 0);
    auto layoutIt = itemTextLayoutCache_.find(layoutKey);
    if (layoutIt == itemTextLayoutCache_.end())
    {
        auto createConfiguredLayout =
            [&](const std::wstring& layoutText,
                float maxWidth,
                float maxHeight,
                ComPtr<IDWriteTextLayout>& result) {
            result.Reset();
            if (FAILED(
                    dwriteFactory_->CreateTextLayout(
                        layoutText.c_str(),
                        static_cast<UINT32>(
                            layoutText.size()),
                        itemTextFormat_.Get(),
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
                itemFontSize_ * layoutScale,
                range);
            if (lightTheme)
            {
                const auto weight =
                    static_cast<
                        DWRITE_FONT_WEIGHT>(
                        std::max<int>(
                            100,
                            static_cast<int>(
                                itemFontWeight_) -
                                200));
                result->SetFontWeight(
                    weight, range);
            }
            result->SetLineSpacing(
                DWRITE_LINE_SPACING_METHOD_UNIFORM,
                itemFontSize_ * 7.0f /
                    6.0f * layoutScale,
                itemFontSize_ * 5.0f /
                    6.0f * layoutScale);
            return true;
        };

        std::wstring visibleText = text;
        if (!selected)
        {
            ComPtr<IDWriteTextLayout>
                wrappedMeasureLayout;
            if (createConfiguredLayout(
                    text, tw, 10000.0f,
                    wrappedMeasureLayout))
            {
                UINT32 lineCount = 0;
                wrappedMeasureLayout->
                    GetLineMetrics(
                        nullptr, 0,
                        &lineCount);
                if (lineCount > 2)
                {
                    std::vector<
                        DWRITE_LINE_METRICS>
                        lines(lineCount);
                    UINT32 actualLineCount = 0;
                    if (SUCCEEDED(
                            wrappedMeasureLayout->
                                GetLineMetrics(
                                    lines.data(),
                                    lineCount,
                                    &actualLineCount)))
                    {
                        visibleText.resize(
                            snowdesktop::
                                item_layout_rules::
                                    VisibleTextLengthForLineLimit(
                                        lines.data(),
                                        actualLineCount,
                                        2,
                                        text.size()));
                    }
                }
            }
        }

        ComPtr<IDWriteTextLayout> layout;
        if (!createConfiguredLayout(
                visibleText, tw, th, layout))
            return;

        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        bool isSingleLine = (metrics.lineCount == 1);
        if (!isSingleLine)
        {
            ComPtr<IDWriteTextLayout> measureLayout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                itemTextFormat_.Get(), 10000.0f, 10000.0f, &measureLayout)) && measureLayout)
            {
                const DWRITE_TEXT_RANGE
                    measureRange{
                        0,
                        static_cast<UINT32>(
                            text.size())
                    };
                measureLayout->SetFontSize(
                    itemFontSize_ * layoutScale,
                    measureRange);
                DWRITE_TEXT_METRICS m{};
                measureLayout->GetMetrics(&m);
                isSingleLine = (m.widthIncludingTrailingWhitespace <= tw + 2.0f);
            }
        }
        if (isSingleLine)
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layoutIt = itemTextLayoutCache_.emplace(std::move(layoutKey), std::move(layout)).first;
    }

    float ty = static_cast<float>(textRect.top);
    DWRITE_TEXT_METRICS metrics{};
    layoutIt->second->GetMetrics(&metrics);
    bool isSingleLine = (metrics.lineCount == 1);
    if (!isSingleLine)
    {
        ComPtr<IDWriteTextLayout> measureLayout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()),
            itemTextFormat_.Get(), 10000.0f, 10000.0f, &measureLayout)) && measureLayout)
        {
            const DWRITE_TEXT_RANGE fullRange{
                0, static_cast<UINT32>(text.size()) };
            measureLayout->SetFontSize(itemFontSize_ * layoutScale, fullRange);
            DWRITE_TEXT_METRICS m{};
            measureLayout->GetMetrics(&m);
            isSingleLine = (m.widthIncludingTrailingWhitespace <= tw + 2.0f);
        }
    }
    if (isSingleLine && selected)
    {
        RECT cr = GetItemTextRect(bounds, false);
        float collapsedH = static_cast<float>(cr.bottom - cr.top);
        ty = cr.top + (collapsedH - th) * 0.5f;
    }

    DrawStyledItemTextLayout(
        context, layoutIt->second.Get(), layoutIt->first,
        D2D1::Point2F(static_cast<float>(textRect.left), ty),
        D2D1::SizeF(tw, th), layoutScale, opacity, lightTheme);
}

void DesktopApp::DrawQuickNavItemText(ID2D1RenderTarget* ctx, RECT bounds,
    const std::wstring& text, bool /*selected*/, bool lightTheme)
{
    if (!ctx || !dwriteFactory_ || !quickNavItemTextFormat_ || text.empty())
        return;

    const float fontSize = quickNavItemTextFormat_->GetFontSize();
    const float lineSpacing = std::max(1.0f, std::floor(fontSize * 1.08f));
    const float baseline = std::max(1.0f, std::floor(fontSize * 0.84f));
    const int textHeight = std::max(1, static_cast<int>(std::ceil(lineSpacing * 2.0f)));
    RECT iconRect = GetQuickNavItemIconRect(bounds);
    const int horizontalPad = QuickNavScale(4);
    const int topGap = std::max(1, QuickNavScale(2));
    const int textTop = std::max<LONG>(bounds.top, iconRect.bottom + topGap);
    RECT textRect = MakeRect(
        bounds.left + horizontalPad,
        textTop,
        bounds.right - horizontalPad,
        std::min<LONG>(bounds.bottom, textTop + textHeight));
    if (IsRectEmptyRect(textRect))
        return;

    const float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
    const float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(),
        static_cast<UINT32>(text.size()), quickNavItemTextFormat_.Get(),
        tw, th, &layout)) || !layout)
        return;

    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineSpacing, baseline);
    DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    ComPtr<IDWriteInlineObject> trimmingSign;
    if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(
        quickNavItemTextFormat_.Get(), &trimmingSign)) && trimmingSign)
        layout->SetTrimming(&trimming, trimmingSign.Get());

    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.lineCount == 1)
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    auto getBrush = [&](const D2D1_COLOR_F& color) -> ID2D1SolidColorBrush* {
        const std::uint64_t key = D2DColorBrushKey(color);
        auto it = brushCache_.find(key);
        if (it == brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush)
                return nullptr;
            it = brushCache_.emplace(key, std::move(brush)).first;
        }
        return it->second.Get();
    };

    const QuickNavTheme& theme = lightTheme ? kQuickNavLight : kQuickNavDark;
    ID2D1SolidColorBrush* textBrush = getBrush(ToD2DColor(theme.itemText));
    if (!textBrush)
        return;

    const D2D1_POINT_2F origin = D2D1::Point2F(
        static_cast<float>(textRect.left),
        static_cast<float>(textRect.top));
    if (!lightTheme)
    {
        if (ID2D1SolidColorBrush* shadowBrush =
            getBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.38f)))
        {
            ctx->DrawTextLayout(
                D2D1::Point2F(origin.x + 1.0f, origin.y + 1.0f),
                layout.Get(), shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    ctx->DrawTextLayout(origin, layout.Get(), textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DesktopApp::DrawD2DText(ID2D1RenderTarget* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
    DWRITE_WORD_WRAPPING wordWrapping)
{
    if (!ctx || !format || text.empty() || IsRectEmptyRect(rect)) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const std::uint64_t key = D2DColorBrushKey(color);
    auto it = brushCache_.find(key);
    if (it == brushCache_.end())
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush) return;
        it = brushCache_.emplace(key, std::move(brush)).first;
    }
    if (wordWrapping != DWRITE_WORD_WRAPPING_NO_WRAP && dwriteFactory_)
    {
        const float width = static_cast<float>(
            std::max<LONG>(1, rect.right - rect.left));
        const float height = static_cast<float>(
            std::max<LONG>(1, rect.bottom - rect.top));
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                format, width, height, &layout)) && layout)
        {
            layout->SetWordWrapping(wordWrapping);
            ctx->DrawTextLayout(
                D2D1::Point2F(
                    static_cast<float>(rect.left),
                    static_cast<float>(rect.top)),
                layout.Get(), it->second.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
            return;
        }
    }
    ctx->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        ToD2DRect(rect), it->second.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DesktopApp::DrawD2DTextEllipsis(ID2D1RenderTarget* ctx, const std::wstring& text,
    RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
    DWRITE_TEXT_ALIGNMENT hAlign, DWRITE_PARAGRAPH_ALIGNMENT vAlign, bool ellipsis)
{
    if (!ctx || !format || text.empty() || IsRectEmptyRect(rect) || !dwriteFactory_) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const std::uint64_t key = D2DColorBrushKey(color);
    auto it = brushCache_.find(key);
    if (it == brushCache_.end())
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush) return;
        it = brushCache_.emplace(key, std::move(brush)).first;
    }
    if (it == brushCache_.end() || !it->second) return;

    const float w = static_cast<float>(std::max<LONG>(1, rect.right - rect.left));
    const float h = static_cast<float>(std::max<LONG>(1, rect.bottom - rect.top));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(),
        static_cast<UINT32>(text.size()), format, w, h, &layout)) || !layout)
        return;
    layout->SetTextAlignment(hAlign);
    layout->SetParagraphAlignment(vAlign);
    if (ellipsis)
    {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format, &sign)) && sign)
            layout->SetTrimming(&trimming, sign.Get());
    }
    ctx->DrawTextLayout(
        D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)),
        layout.Get(), it->second.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
