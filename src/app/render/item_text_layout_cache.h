#pragma once

#include "ui/render/app_font.h"

#include <optional>
#include <string>

namespace snowdesktop
{
// The host configures item formats once, then clears its layout entries before
// replacing the format (font collection/fallback, locale, spacing, direction).
// Keep the successful natural-width measurement in that same lifetime. Natural
// CreateTextLayout metrics have no render-target or DPI input; the exact scaled
// font size is the DPI-dependent input here, and width is compared on every use.
class ItemTextFullWidthCache
{
public:
    size_t RetainedTextBytes() const noexcept { return text_.size() * sizeof(wchar_t); }
    struct Result
    {
        bool fitsSingleLine = false;
        bool reused = false;
    };

    Result FitsSingleLine(IDWriteFactory* factory, IDWriteTextFormat* format,
        const std::wstring& text, float fontSize, DWRITE_FONT_WEIGHT weight,
        float availableWidth)
    {
        if (width_ && factory_.Get() == factory && format_.Get() == format &&
            text_ == text && fontSize_ == fontSize && weight_ == weight)
            return {*width_ <= availableWidth + 2.0f, true};

        width_.reset();
        if (!factory || !format) return {};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> measure;
        if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                format, 10000.0f, 10000.0f, &measure)) || !measure)
            return {};

        const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(text.size())};
        const HRESULT sizeResult = measure->SetFontSize(fontSize, range);
        app_fonts::SetWeight(measure.Get(), weight, range);
        DWRITE_TEXT_METRICS metrics{};
        const HRESULT metricsResult = measure->GetMetrics(&metrics);
        if (SUCCEEDED(sizeResult) && SUCCEEDED(metricsResult))
        {
            factory_ = factory;
            format_ = format;
            text_ = text;
            fontSize_ = fontSize;
            weight_ = weight;
            width_ = metrics.widthIncludingTrailingWhitespace;
        }
        // Preserve the previous zero-initialized-metrics fallback for this
        // draw on failure, but retry rather than retaining a failed result.
        return {metrics.widthIncludingTrailingWhitespace <= availableWidth + 2.0f, false};
    }

private:
    Microsoft::WRL::ComPtr<IDWriteFactory> factory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
    std::wstring text_;
    float fontSize_ = 0;
    DWRITE_FONT_WEIGHT weight_ = DWRITE_FONT_WEIGHT_NORMAL;
    std::optional<float> width_;
};

struct ItemTextLayoutCacheEntry
{
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    ItemTextFullWidthCache fullWidth;
};
}
