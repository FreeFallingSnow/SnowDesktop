#include "d2d_drawing.h"

#include "smooth_corner_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace snowdesktop
{
namespace
{
constexpr std::size_t kBrushResetThreshold = 512;

std::uint64_t ColorBrushKey(const D2D1_COLOR_F& color)
{
    const auto quantize = [](float value) -> std::uint64_t {
        return static_cast<std::uint64_t>(std::lround(
            std::clamp(value, 0.0f, 1.0f) * 65535.0f));
    };
    return quantize(color.r) |
        (quantize(color.g) << 16) |
        (quantize(color.b) << 32) |
        (quantize(color.a) << 48);
}

bool IsEmpty(const RECT& rect)
{
    return rect.right <= rect.left || rect.bottom <= rect.top;
}
}

D2D1_RECT_F ToD2DRect(const RECT& rect)
{
    return D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
        static_cast<float>(rect.right), static_cast<float>(rect.bottom));
}

void D2DDrawing::PrepareTarget(ID2D1RenderTarget* target)
{
    if (target != target_ || solidBrushes_.size() >= kBrushResetThreshold)
    {
        solidBrushes_.clear();
        target_ = target;
    }
}

void D2DDrawing::Reset()
{
    solidBrushes_.clear();
    target_ = nullptr;
}

ID2D1SolidColorBrush* D2DDrawing::FindOrCreateBrush(const D2D1_COLOR_F& color)
{
    const auto key = ColorBrushKey(color);
    auto found = solidBrushes_.find(key);
    if (found == solidBrushes_.end())
    {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(color, &brush)) || !brush)
            return nullptr;
        found = solidBrushes_.emplace(key, std::move(brush)).first;
    }
    return found->second.Get();
}

Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> D2DDrawing::GetSolidBrush(
    ID2D1RenderTarget* target, const D2D1_COLOR_F& color)
{
    if (!target) return {};
    PrepareTarget(target);
    return Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>{FindOrCreateBrush(color)};
}

void D2DDrawing::DrawRoundedRectangle(ID2D1RenderTarget* target, RECT rect, float radius,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth)
{
    if (!target || IsEmpty(rect)) return;
    DrawRoundedRectangle(target, ToD2DRect(rect), radius, fill, stroke, strokeWidth);
}

void D2DDrawing::DrawRoundedRectangle(ID2D1RenderTarget* target, D2D1_RECT_F rect,
    float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth)
{
    if (!target || rect.right <= rect.left || rect.bottom <= rect.top) return;
    PrepareTarget(target);

    if (fill.a > 0.0f)
    {
        auto* brush = FindOrCreateBrush(fill);
        if (!brush) return;
        smooth_corners::Fill(target, rect, radius, brush);
    }
    if (stroke.a > 0.0f)
    {
        if (auto* brush = FindOrCreateBrush(stroke))
            smooth_corners::Stroke(target, rect, radius, brush, strokeWidth);
    }
}

void D2DDrawing::DrawFilledRectangle(ID2D1RenderTarget* target, RECT rect,
    D2D1_COLOR_F fill, D2D1_COLOR_F stroke)
{
    if (!target || IsEmpty(rect)) return;
    PrepareTarget(target);

    const auto bounds = ToD2DRect(rect);
    if (fill.a > 0.0f)
    {
        if (auto* brush = FindOrCreateBrush(fill))
            target->FillRectangle(bounds, brush);
    }
    if (stroke.a > 0.0f)
    {
        if (auto* brush = FindOrCreateBrush(stroke))
            target->DrawRectangle(bounds, brush, 1.0f, nullptr);
    }
}

void D2DDrawing::DrawSeparator(
    ID2D1RenderTarget* target, RECT rect, const D2D1_COLOR_F& color)
{
    if (!target || IsEmpty(rect)) return;
    PrepareTarget(target);
    if (auto* brush = FindOrCreateBrush(color))
        target->FillRectangle(ToD2DRect(rect), brush);
}

void D2DDrawing::DrawBasicText(ID2D1RenderTarget* target, IDWriteFactory* factory,
    const std::wstring& text, RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
    DWRITE_WORD_WRAPPING wordWrapping)
{
    if (!target || !format || text.empty() || IsEmpty(rect)) return;
    PrepareTarget(target);
    auto* brush = FindOrCreateBrush(color);
    if (!brush) return;

    if (wordWrapping != DWRITE_WORD_WRAPPING_NO_WRAP && factory)
    {
        const float width = static_cast<float>(std::max<LONG>(1, rect.right - rect.left));
        const float height = static_cast<float>(std::max<LONG>(1, rect.bottom - rect.top));
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                format, width, height, &layout)) && layout)
        {
            layout->SetWordWrapping(wordWrapping);
            target->DrawTextLayout(D2D1::Point2F(
                    static_cast<float>(rect.left), static_cast<float>(rect.top)),
                layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            return;
        }
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        ToD2DRect(rect), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void D2DDrawing::DrawTextEllipsis(ID2D1RenderTarget* target, IDWriteFactory* factory,
    const std::wstring& text, RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color,
    DWRITE_TEXT_ALIGNMENT hAlign, DWRITE_PARAGRAPH_ALIGNMENT vAlign, bool ellipsis,
    IDWriteTextLayout* cachedLayout)
{
    if (!target || !format || text.empty() || IsEmpty(rect) || !factory) return;
    PrepareTarget(target);
    auto* brush = FindOrCreateBrush(color);
    if (!brush) return;

    const auto origin =
        D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top));
    if (cachedLayout)
    {
        target->DrawTextLayout(origin, cachedLayout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }

    const float width = static_cast<float>(std::max<LONG>(1, rect.right - rect.left));
    const float height = static_cast<float>(std::max<LONG>(1, rect.bottom - rect.top));
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            format, width, height, &layout)) || !layout)
        return;
    layout->SetTextAlignment(hAlign);
    layout->SetParagraphAlignment(vAlign);
    if (ellipsis)
    {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &sign)) && sign)
            layout->SetTrimming(&trimming, sign.Get());
    }
    target->DrawTextLayout(origin, layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
}
