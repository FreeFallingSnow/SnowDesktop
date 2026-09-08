#pragma once
#include "large_icon_render_rules.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

namespace snowdesktop::large_icon_render_rules
{
// Use identical font metrics in settings eligibility and desktop rendering.
// Width is measured after wrapping; short names never reserve a fixed column.
inline MeasureTitle MeasureTitleText(IDWriteFactory* factory, const LargeIconConfig& config,
    std::wstring_view name, double scale)
{
    using Microsoft::WRL::ComPtr;
    if (!factory || name.empty() || scale <= 0) return {};
    const float size = static_cast<float>(config.revealTitleSize * scale);
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, static_cast<DWRITE_FONT_WEIGHT>(config.titleWeight),
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return {};
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.3f, size);
    ComPtr<IDWriteFactory> owner = factory;
    std::wstring text(name);
    ComPtr<IDWriteTextLayout> natural;
    DWRITE_TEXT_METRICS full{};
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(),
        100000, 100000, &natural)) || FAILED(natural->GetMetrics(&full))) return {};
    const double minimum = std::min<double>(std::ceil(full.width) + 2 * scale, 2 * size);
    return [owner, format, text = std::move(text), size, scale, minimum](double width, double height) -> TitleSize {
        const double line = size * 1.3;
        if (width < minimum || height + .01 < line) return {};
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(owner->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(),
            static_cast<float>(width), static_cast<float>(height), &layout)) || FAILED(layout->GetMetrics(&metrics))) return {};
        const double lines = std::min({2., static_cast<double>(metrics.lineCount), std::floor((height + .01) / line)});
        // Keep the wrapping width for truncated text; its ellipsis needs room.
        const double measured = metrics.lineCount > lines ? width : std::min(width, std::ceil(metrics.width) + 2 * scale);
        return {measured, std::ceil(line * lines), lines >= 1};
    };
}
}
