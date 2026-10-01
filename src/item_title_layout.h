#pragma once
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>

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
    return layout->SetTrimming(&trimming, ellipsis.Get());
}
}
