#pragma once
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>

namespace snowdesktop
{
// Keep the complete text for shaping, surrogate pairs and explicit newlines.
// DirectWrite replaces only the overflowing end of the final visible line
// with the format's native compact ellipsis glyph, without added whitespace.
inline HRESULT TrimItemTitle(IDWriteFactory* factory, IDWriteTextLayout* layout,
    IDWriteTextFormat* format, int lines, float lineHeight)
{
    if (!factory || !layout || !format || lineHeight <= 0) return E_INVALIDARG;
    auto result = layout->SetMaxHeight(lineHeight * static_cast<float>(std::clamp(lines, 1, 2)));
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    result = factory->CreateEllipsisTrimmingSign(format, &ellipsis);
    if (FAILED(result)) return result;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    result = layout->SetTrimming(&trimming, ellipsis.Get());
    if (FAILED(result)) return result;
    // DirectWrite trims soft wraps within a paragraph, but a hard newline
    // can hide the following paragraph without adding a sign. Replace that
    // break and its hidden remainder with the same native ellipsis object.
    UINT32 count = 0;
    layout->GetLineMetrics(nullptr, 0, &count);
    std::vector<DWRITE_LINE_METRICS> metrics(count);
    result = layout->GetLineMetrics(metrics.data(), count, &count);
    if (FAILED(result)) return result;
    const auto limit = static_cast<UINT32>(std::clamp(lines, 1, 2));
    if (count > limit && metrics[limit - 1].newlineLength > 0)
    {
        UINT32 start = 0, total = 0;
        for (UINT32 i = 0; i < count; ++i)
        {
            total += metrics[i].length;
            if (i < limit) start += metrics[i].length;
        }
        start -= metrics[limit - 1].newlineLength;
        result = layout->SetInlineObject(ellipsis.Get(), {start, total - start});
    }
    return result;
}
}
