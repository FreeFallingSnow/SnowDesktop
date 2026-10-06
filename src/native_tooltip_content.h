#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <dwrite.h>
#include <wrl/client.h>

namespace snowdesktop
{
// Tooltip text contract extracted from DrawWidgetViewTooltip. Callers cache
// their normal 13-DIP wrapping format and provide their own surface limits.
struct NativeTooltipTextLayout
{
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    float width = 0;
    float height = 0;
};
inline HRESULT MeasureNativeTooltip(IDWriteFactory* factory, IDWriteTextFormat* format,
    std::wstring_view title, std::wstring_view body, float maximumWidth,
    float maximumHeight, NativeTooltipTextLayout& result)
{
    result = {};
    if (!factory || !format || body.empty()) return E_INVALIDARG;
    maximumWidth = std::max(1.f, maximumWidth);
    maximumHeight = std::max(1.f, maximumHeight);
    std::wstring content(title);
    if (!content.empty()) content.push_back(L'\n');
    content += body;
    auto status = factory->CreateTextLayout(content.data(), static_cast<UINT32>(content.size()),
        format, std::max(1.f, maximumWidth - 16.f), std::max(1.f, maximumHeight - 12.f), &result.layout);
    if (FAILED(status)) return status;
    if (!title.empty())
    {
        const DWRITE_TEXT_RANGE heading{0, static_cast<UINT32>(title.size())};
        result.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, heading);
        result.layout->SetFontSize(14.f, heading);
    }
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &ellipsis)))
        result.layout->SetTrimming(&trimming, ellipsis.Get());
    DWRITE_TEXT_METRICS metrics{};
    status = result.layout->GetMetrics(&metrics);
    if (FAILED(status)) { result = {}; return status; }
    result.width = std::min(maximumWidth, std::max(32.f,
        std::ceil(metrics.widthIncludingTrailingWhitespace) + 16.f));
    result.height = std::min(maximumHeight, std::max(24.f, std::ceil(metrics.height) + 12.f));
    return S_OK;
}
}
