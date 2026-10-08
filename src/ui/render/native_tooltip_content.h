#pragma once
#include "app_font.h"
#include "native_tooltip_preferences.h"
#include "theme/panel_gradient_renderer.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <dwrite.h>
#include <wrl/client.h>
#include <d2d1_1.h>

namespace snowdesktop
{
// Both independent popup hosts and inline renderers use this presentation
// contract. Geometry and lifetime remain with the owner of each surface.
struct NativeTooltipLayoutOptions
{
    float scale = 1.f;
    float paddingX = 12.f, paddingY = 7.f;
    float minimumWidth = 32.f, minimumHeight = 32.f;
    bool centered = false;
};
inline HRESULT CreateNativeTooltipTextFormat(IDWriteFactory* factory,
    IDWriteTextFormat** format, float scale = 1.f, bool wrap = true)
{
    if (!factory || !format) return E_INVALIDARG;
    const auto result = app_fonts::CreateTextFormat(factory, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, NativeTooltipFontSize() * scale, L"", format);
    if (SUCCEEDED(result)) (*format)->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return result;
}
struct NativeTooltipTextLayout
{
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    float width = 0;
    float height = 0;
    float paddingX = 12.f, paddingY = 7.f;
};
inline HRESULT MeasureNativeTooltip(IDWriteFactory* factory, IDWriteTextFormat* format,
    std::wstring_view title, std::wstring_view body, float maximumWidth,
    float maximumHeight, NativeTooltipTextLayout& result, NativeTooltipLayoutOptions options = {})
{
    result = {};
    if (!factory || !format || body.empty()) return E_INVALIDARG;
    maximumWidth = std::max(1.f, maximumWidth);
    maximumHeight = std::max(1.f, maximumHeight);
    result.paddingX = std::min(options.paddingX * options.scale, maximumWidth * .25f);
    result.paddingY = std::min(options.paddingY * options.scale, maximumHeight * .25f);
    std::wstring content(title);
    if (!content.empty()) content.push_back(L'\n');
    content += body;
    auto status = factory->CreateTextLayout(content.data(), static_cast<UINT32>(content.size()),
        format, std::max(1.f, maximumWidth - result.paddingX * 2.f),
        std::max(1.f, maximumHeight - result.paddingY * 2.f), &result.layout);
    if (FAILED(status)) return status;
    if (!title.empty())
    {
        const DWRITE_TEXT_RANGE heading{0, static_cast<UINT32>(title.size())};
        result.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, heading);
    }
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &ellipsis)))
        result.layout->SetTrimming(&trimming, ellipsis.Get());
    DWRITE_TEXT_METRICS metrics{};
    status = result.layout->GetMetrics(&metrics);
    if (FAILED(status)) { result = {}; return status; }
    result.width = std::min(maximumWidth, std::max(options.minimumWidth * options.scale,
        std::ceil(metrics.widthIncludingTrailingWhitespace) + result.paddingX * 2.f));
    result.height = std::min(maximumHeight, std::max(options.minimumHeight * options.scale,
        std::ceil(metrics.height) + result.paddingY * 2.f));
    result.layout->SetMaxWidth(std::max(1.f, result.width - result.paddingX * 2.f));
    result.layout->SetMaxHeight(std::max(1.f, result.height - result.paddingY * 2.f));
    if (options.centered)
    {
        result.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        result.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return S_OK;
}
inline bool NativeTooltipHighContrast()
{
    HIGHCONTRASTW value{sizeof(value)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
        (value.dwFlags & HCF_HIGHCONTRASTON);
}
inline D2D1_COLOR_F NativeTooltipSystemColor(int index)
{
    const auto color = GetSysColor(index);
    return D2D1::ColorF(GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f);
}
inline void DrawNativeTooltipText(ID2D1RenderTarget* context, D2D1_RECT_F bounds,
    const NativeTooltipTextLayout& measured, const PersonalizationSettings& appearance)
{
    if (!context || !measured.layout) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    const auto foreground = NativeTooltipHighContrast() ? NativeTooltipSystemColor(COLOR_INFOTEXT) :
        D2D1::ColorF(appearance.contentTheme == 1 ? 0x202020 : 0xf4f4f4);
    if (FAILED(context->CreateSolidColorBrush(foreground, &brush))) return;
    const auto content = D2D1::RectF(bounds.left + measured.paddingX, bounds.top + measured.paddingY,
        std::max(bounds.left + measured.paddingX, bounds.right - measured.paddingX),
        std::max(bounds.top + measured.paddingY, bounds.bottom - measured.paddingY));
    context->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_ALIASED);
    context->DrawTextLayout(D2D1::Point2F(content.left, content.top), measured.layout.Get(),
        brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    context->PopAxisAlignedClip();
}
inline void DrawNativeTooltip(ID2D1RenderTarget* context, D2D1_RECT_F bounds,
    const NativeTooltipTextLayout& measured, const PersonalizationSettings& appearance, float scale = 1.f)
{
    if (!context || !measured.layout) return;
    const bool highContrast = NativeTooltipHighContrast();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    const auto background = highContrast ? NativeTooltipSystemColor(COLOR_INFOBK) :
        D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha);
    if (FAILED(context->CreateSolidColorBrush(background, &brush))) return;
    const float radius = NativeTooltipCornerRadius(appearance, bounds.right - bounds.left, bounds.bottom - bounds.top, scale);
    const auto shape = D2D1::RoundedRect(bounds, radius, radius);
    if (highContrast || !DrawPanelGradient(context, bounds, radius, appearance.panelGradient))
        context->FillRoundedRectangle(shape, brush.Get());
    brush->SetColor(highContrast ? NativeTooltipSystemColor(COLOR_INFOTEXT) :
        D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha));
    context->DrawRoundedRectangle(shape, brush.Get(), highContrast ? 1.f : appearance.widgetBorderWidth * scale);
    DrawNativeTooltipText(context, bounds, measured, appearance);
}
}
