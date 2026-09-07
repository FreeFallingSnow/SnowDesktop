#include "large_icon_renderer.h"
#include "large_icon_render_rules.h"
#include <d2d1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace snowdesktop::large_icon_renderer
{
using Microsoft::WRL::ComPtr;
namespace
{
D2D1_COLOR_F Color(unsigned rgb, double opacity) { return D2D1::ColorF(rgb, static_cast<float>(opacity)); }
D2D1_RECT_F Rect(RECT rect) { return D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right), static_cast<float>(rect.bottom)); }
void Rounded(ID2D1RenderTarget* target, D2D1_RECT_F rect, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float width = 1)
{
    const auto shape = D2D1::RoundedRect(rect, radius, radius);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (fill.a > 0 && SUCCEEDED(target->CreateSolidColorBrush(fill, &brush))) target->FillRoundedRectangle(shape, brush.Get());
    brush.Reset();
    if (stroke.a > 0 && width > 0 && SUCCEEDED(target->CreateSolidColorBrush(stroke, &brush))) target->DrawRoundedRectangle(shape, brush.Get(), width);
}
ComPtr<IDWriteTextFormat> Format(IDWriteFactory* fonts, const LargeIconConfig& config, float scale)
{
    ComPtr<IDWriteTextFormat> format;
    if (!fonts) return format;
    fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(config.titleSize) * scale, L"", &format);
    if (format)
    {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
        format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
            static_cast<float>(config.titleSize) * scale * 1.3f, static_cast<float>(config.titleSize) * scale);
    }
    return format;
}
large_icon_render_rules::ContentGeometry Content(const LargeIconConfig& config, const View& view)
{
    const auto size = view.bitmap ? view.bitmap->GetSize() : D2D1_SIZE_F{};
    return large_icon_render_rules::ResolveContent(config, view.frame.right - view.frame.left, view.frame.bottom - view.frame.top,
        size.width, size.height, view.scale, view.original, view.animations, view.hover, view.pressed, view.launchWave);
}
}

void DrawFrame(ID2D1RenderTarget* target, IDWriteFactory* fonts, const LargeIconConfig& config, const View& view)
{
    if (!target || view.frame.right <= view.frame.left || view.frame.bottom <= view.frame.top) return;
    const auto frame = Rect(view.frame);
    const float scale = view.scale, hover = view.hover;
    const float radius = std::min(static_cast<float>(config.radius) * scale,
        std::min(frame.right - frame.left, frame.bottom - frame.top) / 2);
    const auto geometry = Content(config, view);
    if (config.shadow || (config.hoverFrame == 3 && hover > 0))
        for (int spread = 5; spread >= 1; --spread)
        {
            const float extent = spread * scale;
            const auto shadow = D2D1::RectF(frame.left - extent, frame.top - extent + 2 * scale,
                frame.right + extent, frame.bottom + extent + 2 * scale);
            Rounded(target, shadow, radius + extent, Color(0, view.opacity *
                (config.shadowStrength * (config.shadow ? .06 : 0) + (config.hoverFrame == 3 ? hover * .035 * config.amplitude : 0))), Color(0, 0));
        }
    Rounded(target, frame, radius, Color(large_icon_render_rules::Background(config, view.neutral, view.accent),
        view.opacity * (config.opacity + (config.hoverFrame == 1 ? (config.hoverOpacity - config.opacity) * hover : 0))), Color(0, 0));

    ComPtr<ID2D1Factory> factory; target->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    if (factory) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame, radius, radius), &clip);
    if (clip) target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
    const auto image = D2D1::RectF(frame.left + static_cast<float>(geometry.x), frame.top + static_cast<float>(geometry.y),
        frame.left + static_cast<float>(geometry.x + geometry.width), frame.top + static_cast<float>(geometry.y + geometry.height));
    if (view.bitmap) target->DrawBitmap(view.bitmap, image, view.opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    else if (view.placeholder)
        view.placeholder({static_cast<LONG>(std::lround(image.left)), static_cast<LONG>(std::lround(image.top)),
            static_cast<LONG>(std::lround(image.right)), static_cast<LONG>(std::lround(image.bottom))}, view.opacity);

    if ((geometry.leftReveal || (!view.original && config.coverHover == 2)) && hover > 0)
    {
        const float lineHeight = static_cast<float>(config.titleSize) * scale * 1.3f;
        // A fractional rectangle can lose a few ulps after adding its origin;
        // DirectWrite then fits only one line into an apparent two-line box.
        const float titleHeight = std::ceil(lineHeight * 2);
        const float top = geometry.leftReveal ? (frame.top + frame.bottom) / 2 - titleHeight / 2 : frame.bottom - titleHeight - 6 * scale;
        const auto title = D2D1::RectF(frame.left + static_cast<float>(geometry.leftReveal ? geometry.titleLeft : 6 * scale),
            top, frame.right - 12 * scale, top + titleHeight);
        const unsigned textColor = config.autoTitleColor ? 0xffffff : config.titleColor;
        Rounded(target, title, 4 * scale, Color(large_icon_render_rules::TitleBackdrop(textColor), .88 * hover), Color(0, 0));
        auto format = Format(fonts, config, scale);
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(Color(textColor, hover), &brush);
        if (format && brush && title.right > title.left + 12 * scale)
        {
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis; fonts->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
            format->SetTrimming(&trimming, ellipsis.Get());
            auto text = title; text.left += (6 + (1 - hover) * 6) * scale; text.right -= 6 * scale;
            target->DrawText(view.name.data(), static_cast<UINT32>(view.name.size()), format.Get(), text, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    if (clip) target->PopLayer();
    if (config.border || (config.hoverFrame == 2 && hover > 0))
        Rounded(target, frame, radius, Color(0, 0), Color(config.borderColor,
            view.opacity * std::min(1., (config.border ? config.borderOpacity : 0) + (config.hoverFrame == 2 ? hover * .4 * config.amplitude : 0))),
            static_cast<float>(config.borderWidth) * scale);
    if (view.animations && config.launch == 2 && view.launchWave > 0)
        Rounded(target, frame, radius, Color(0, 0), Color(0xffffff, std::min(1., view.launchWave * .65 * config.amplitude)), 2 * scale);
    if (view.selected)
    {
        Rounded(target, frame, radius, Color(0, 0), Color(0x75baff, .95), scale);
        Rounded(target, D2D1::RectF(frame.left + 5 * scale, frame.top + 5 * scale, frame.left + 13 * scale, frame.top + 13 * scale),
            4 * scale, Color(0x75baff, 1), Color(0x75baff, 1));
    }
}

void DrawFloatingTitle(ID2D1RenderTarget* target, IDWriteFactory* fonts, const LargeIconConfig& config, const View& view, RECT workArea)
{
    if (!target || view.hover <= .01f || workArea.right <= workArea.left || workArea.bottom <= workArea.top) return;
    const auto geometry = Content(config, view);
    auto format = Format(fonts, config, view.scale);
    if (!format) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    const float padding = 6 * view.scale;
    if (geometry.leftReveal)
    {
        ComPtr<IDWriteTextLayout> inner;
        DWRITE_TEXT_METRICS metrics{};
        const float width = static_cast<float>(view.frame.right - view.frame.left - geometry.titleLeft) - 24 * view.scale;
        if (width > 0 && SUCCEEDED(fonts->CreateTextLayout(view.name.data(), static_cast<UINT32>(view.name.size()), format.Get(), width, 100000.f, &inner)) &&
            SUCCEEDED(inner->GetMetrics(&metrics)) && metrics.lineCount <= 2) return;
    }
    const int width = std::min<LONG>(workArea.right - workArea.left, std::max(160, static_cast<int>(260 * view.scale)));
    ComPtr<IDWriteTextLayout> text;
    if (FAILED(fonts->CreateTextLayout(view.name.data(), static_cast<UINT32>(view.name.size()), format.Get(),
        std::max(1.f, width - padding * 2), 100000.f, &text))) return;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(text->GetMetrics(&metrics))) return;
    const int height = std::min<LONG>(workArea.bottom - workArea.top, static_cast<int>(std::ceil(metrics.height + padding * 2)));
    const LONG left = std::clamp((view.frame.left + view.frame.right - width) / 2, workArea.left, workArea.right - width);
    LONG top = view.frame.bottom + static_cast<LONG>(5 * view.scale);
    if (top + height > workArea.bottom) top = view.frame.top - height - static_cast<LONG>(5 * view.scale);
    top = std::clamp(top, workArea.top, workArea.bottom - height);
    const auto title = Rect({left, top, left + width, top + height});
    const unsigned textColor = config.autoTitleColor ? 0xffffff : config.titleColor;
    Rounded(target, title, 6 * view.scale, Color(large_icon_render_rules::TitleBackdrop(textColor), .96 * view.hover), Color(0xffffff, .18 * view.hover), view.scale);
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(Color(textColor, view.hover), &brush);
    if (brush)
    {
        target->PushAxisAlignedClip(title, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target->DrawTextLayout(D2D1::Point2F(title.left + padding, title.top + padding), text.Get(), brush.Get());
        target->PopAxisAlignedClip();
    }
}
}
