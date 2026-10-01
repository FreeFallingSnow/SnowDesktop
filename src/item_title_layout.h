#pragma once
#include <dwrite_1.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <vector>

namespace snowdesktop
{
class CompactTitleEllipsis final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteInlineObject>
{
public:
    HRESULT Initialize(IDWriteFactory* factory, IDWriteTextFormat* format)
    {
        auto result = factory->CreateTextLayout(L"...", 3, format,
            format->GetFontSize() * 3, format->GetFontSize() * 3, &layout_);
        if (FAILED(result)) return result;
        layout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Microsoft::WRL::ComPtr<IDWriteTextLayout1> spacing;
        result = layout_.As(&spacing);
        if (FAILED(result)) return result;
        // Only the sign is tightened. Three Segoe periods with this tracking
        // occupy about half an em, rather than the CJK ellipsis's full em.
        result = spacing->SetCharacterSpacing(0, -format->GetFontSize() * .06f, 0, {0, 3});
        if (FAILED(result)) return result;
        DWRITE_TEXT_METRICS text{};
        result = layout_->GetMetrics(&text);
        if (FAILED(result)) return result;
        DWRITE_LINE_METRICS line{};
        UINT32 count = 0;
        result = layout_->GetLineMetrics(&line, 1, &count);
        if (FAILED(result)) return result;
        result = layout_->GetOverhangMetrics(&overhang_);
        if (FAILED(result)) return result;
        leadingGap_ = format->GetFontSize() * .1f;
        metrics_ = {leadingGap_ + text.widthIncludingTrailingWhitespace + std::max(0.0f, overhang_.right),
            text.height, line.baseline, FALSE};
        return S_OK;
    }

    IFACEMETHODIMP Draw(void* context, IDWriteTextRenderer* renderer,
        FLOAT x, FLOAT y, BOOL, BOOL, IUnknown*) override
    {
        // DirectWrite supplies the inline object's top-left, with its baseline
        // already aligned. Subtracting the baseline draws the sign a line early.
        return layout_->Draw(context, renderer, x + leadingGap_, y);
    }
    IFACEMETHODIMP GetMetrics(DWRITE_INLINE_OBJECT_METRICS* result) override
    { if (!result) return E_POINTER; *result = metrics_; return S_OK; }
    IFACEMETHODIMP GetOverhangMetrics(DWRITE_OVERHANG_METRICS* result) override
    { if (!result) return E_POINTER; *result = overhang_; result->right = 0; return S_OK; }
    IFACEMETHODIMP GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) override
    {
        if (!before || !after) return E_POINTER;
        *before = *after = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        return S_OK;
    }
private:
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout_;
    DWRITE_INLINE_OBJECT_METRICS metrics_{};
    DWRITE_OVERHANG_METRICS overhang_{};
    float leadingGap_ = 0;
};

// Keep the complete text for shaping, surrogate pairs and explicit newlines.
// DirectWrite replaces only the overflowing end of the final visible line
// with three tightly tracked periods and a small gap before the sign.
inline HRESULT TrimItemTitle(IDWriteFactory* factory, IDWriteTextLayout* layout,
    IDWriteTextFormat* format, int lines, float lineHeight, bool showEllipsis = true)
{
    if (!factory || !layout || !format || lineHeight <= 0) return E_INVALIDARG;
    // Ordinary WRAP trims an overlong English word on an earlier line even
    // when another line is available. Desktop labels wrap inside such words.
    auto result = layout->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
    if (FAILED(result)) return result;
    result = layout->SetMaxHeight(lineHeight * static_cast<float>(std::clamp(lines, 1, 2)));
    if (FAILED(result)) return result;
    UINT32 untrimmedCount = 0;
    layout->GetLineMetrics(nullptr, 0, &untrimmedCount);
    if (!showEllipsis || untrimmedCount <= static_cast<UINT32>(std::clamp(lines, 1, 2)))
    {
        const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        return layout->SetTrimming(&none, nullptr);
    }
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    // CJK families often give U+2026 a full em of advance. Use Windows' own
    // compact sign at the requested size/weight so font switching does not
    // insert a full-character gap before a truncated title's final dots.
    Microsoft::WRL::ComPtr<IDWriteTextFormat> signFormat;
    result = factory->CreateTextFormat(L"Segoe UI", nullptr, format->GetFontWeight(),
        format->GetFontStyle(), format->GetFontStretch(), format->GetFontSize(), L"", &signFormat);
    if (FAILED(result)) return result;
    const auto compact = Microsoft::WRL::Make<CompactTitleEllipsis>();
    if (!compact) return E_OUTOFMEMORY;
    result = compact->Initialize(factory, signFormat.Get());
    if (FAILED(result)) return result;
    ellipsis = compact;
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
