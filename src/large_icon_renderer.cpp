#include "large_icon_renderer.h"
#include "panel_gradient_renderer.h"
#include "large_icon_title_measure.h"
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace snowdesktop::large_icon_renderer
{
using Microsoft::WRL::ComPtr;
namespace
{
D2D1_RECT_F Rect(RECT r)
{ return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom)); }

void DrawShine(ID2D1RenderTarget* target, const LargeIconConfig& c, const View& view,
    const D2D1_RECT_F& image, const D2D1_RECT_F& source)
{
    if (c.effect != 5 || !view.animations || !view.bitmap || view.shine < 0 || view.shine >= 1 ||
        c.shineStrength <= 0 || image.right <= image.left || image.bottom <= image.top ||
        source.right <= source.left || source.bottom <= source.top) return;
    // Use the same source crop and destination transform as DrawBitmap. The
    // alpha mask prevents the sweep from painting transparent image margins.
    ComPtr<ID2D1BitmapBrush> mask;
    const auto transform = D2D1::Matrix3x2F::Scale(
        (image.right - image.left) / (source.right - source.left),
        (image.bottom - image.top) / (source.bottom - source.top));
    auto mapping = transform;
    mapping._31 = image.left - source.left * transform._11;
    mapping._32 = image.top - source.top * transform._22;
    if (FAILED(target->CreateBitmapBrush(view.bitmap, D2D1::BitmapBrushProperties(),
            D2D1::BrushProperties(1, mapping), &mask))) return;
    ComPtr<ID2D1GradientStopCollection> stops;
    const D2D1_GRADIENT_STOP colors[]{{0, D2D1::ColorF(0xffffff, 0.f)},
        {.5f, D2D1::ColorF(0xffffff, static_cast<float>(c.shineStrength))}, {1, D2D1::ColorF(0xffffff, 0.f)}};
    if (FAILED(target->CreateGradientStopCollection(colors, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops))) return;
    const float width = image.right - image.left, height = image.bottom - image.top;
    const float band = std::min(width, height) * .45f;
    const float start = -.34f * height - band / 2;
    const float center = start + (.94f * width + band / 2 - start) * view.shine;
    const auto point = [&](float distance) { return D2D1::Point2F(image.left + .94f * distance, image.top - .34f * distance); };
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (FAILED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            point(center - band / 2), point(center + band / 2)), stops.Get(), &brush))) return;
    ComPtr<ID2D1Layer> layer;
    if (FAILED(target->CreateLayer(nullptr, &layer))) return;
    target->PushLayer(D2D1::LayerParameters(image, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::Matrix3x2F::Identity(), view.opacity, mask.Get()), layer.Get());
    target->FillRectangle(image, brush.Get());
    target->PopLayer();
}

void DrawEdgeGlow(ID2D1RenderTarget* target, const LargeIconConfig& c, const View& view,
    const D2D1_RECT_F& frame, float radius)
{
    if (c.effect != 4 || view.hover <= 0 || c.glowStrength <= 0) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    const unsigned color = large_icon_render_rules::Mix(view.accent ? view.accent : 0x75baff, 0xffffff, .3);
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(color), &brush))) return;
    const float depth = std::min(10.f * view.scale, std::min(frame.right - frame.left, frame.bottom - frame.top) * .08f);
    constexpr int steps = 12;
    const float stroke = depth / steps;
    for (int i = 0; i < steps; ++i)
    {
        const float inset = (i + .5f) * stroke;
        const auto inner = D2D1::RectF(frame.left + inset, frame.top + inset, frame.right - inset, frame.bottom - inset);
        const float innerRadius = std::max(0.f, radius - inset);
        brush->SetOpacity(static_cast<float>(c.glowStrength) * view.hover * view.opacity * std::exp(-3.f * i / (steps - 1)));
        target->DrawRoundedRectangle(D2D1::RoundedRect(inner, innerRadius, innerRadius), brush.Get(), stroke);
    }
}
}
void DrawFrame(ID2D1RenderTarget* target, IDWriteFactory* fonts, const LargeIconConfig& c, const View& view)
{
    if (!target || view.frame.right <= view.frame.left || view.frame.bottom <= view.frame.top) return;
    const auto frame = Rect(view.frame);
    const float radius = static_cast<float>(large_icon_render_rules::Radius(c,
        frame.right - frame.left, frame.bottom - frame.top, view.scale));
    const bool fill = IsLargeIconFill(c);
    auto background = view.backgroundResolved ? view.background :
        large_icon_render_rules::DefaultBackground(c, view.accent, view.hasEdgeColor, view.edgeColor);
    if (!fill)
    {
        if (view.drawBackground) view.drawBackground(target, view.frame, radius, view.opacity);
        else if (!DrawPanelGradient(target, frame, radius, background.gradient, view.opacity))
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(background.color,
                    static_cast<float>(background.opacity) * view.opacity), &brush)))
                target->FillRoundedRectangle(D2D1::RoundedRect(frame, radius, radius), brush.Get());
        }
    }
    else if (!view.bitmap)
    {
        // A missing asset is different from an intentionally transparent image.
        // Keep loading/failed items visible without invoking component material
        // or a separate foreground layer. Loaded images retain their own alpha.
        ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(view.neutral, .65f * view.opacity), &brush)))
            target->FillRoundedRectangle(D2D1::RoundedRect(frame, radius, radius), brush.Get());
    }

    ComPtr<ID2D1Factory> factory; target->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    if (factory) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame, radius, radius), &clip);
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Layer> layer;
    // Automatic layers are supported by device contexts. WIC render targets
    // need an explicit layer to apply the same rounded mask.
    if (clip && FAILED(target->QueryInterface(IID_PPV_ARGS(&context))) &&
        FAILED(target->CreateLayer(nullptr, &layer))) return;
    if (clip) target->PushLayer(D2D1::LayerParameters(frame, clip.Get()), layer.Get());
    else target->PushAxisAlignedClip(frame, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const auto sourceSize = view.bitmap ? view.bitmap->GetSize() : D2D1_SIZE_F{};
    const auto geometry = large_icon_render_rules::ResolveContent(c, frame.right - frame.left, frame.bottom - frame.top,
        sourceSize.width, sourceSize.height, view.scale, view.original, c.effect == 3 && !view.animations ? 0 : view.hover,
        c.effect == 2 ? large_icon_render_rules::MeasureTitleText(fonts, c, view.name, view.scale) : large_icon_render_rules::MeasureTitle{});
    const auto image = D2D1::RectF(frame.left + static_cast<float>(geometry.x), frame.top + static_cast<float>(geometry.y),
        frame.left + static_cast<float>(geometry.x + geometry.width), frame.top + static_cast<float>(geometry.y + geometry.height));
    const auto source = D2D1::RectF(static_cast<float>(geometry.sourceX), static_cast<float>(geometry.sourceY),
        static_cast<float>(geometry.sourceX + geometry.sourceWidth), static_cast<float>(geometry.sourceY + geometry.sourceHeight));
    // Crop before translating the fill layer: source overscan must not refill
    // the transparent area revealed for an inner title.
    if (view.bitmap)
    {
        if (context) context->DrawBitmap(view.bitmap, &image, view.opacity,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, geometry.cropped ? &source : nullptr, nullptr);
        else target->DrawBitmap(view.bitmap, image, view.opacity,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, geometry.cropped ? &source : nullptr);
        DrawShine(target, c, view, image, source);
    }
    else if (view.placeholder && !fill)
        view.placeholder(target, {static_cast<LONG>(std::lround(image.left)), static_cast<LONG>(std::lround(image.top)),
            static_cast<LONG>(std::lround(image.right)), static_cast<LONG>(std::lround(image.bottom))}, view.opacity);

    if (fonts && !view.name.empty() && (geometry.leftReveal || geometry.upReveal) && view.hover > 0)
    {
        const float size = static_cast<float>(c.revealTitleSize) * view.scale;
        ComPtr<IDWriteTextFormat> format;
        fonts->CreateTextFormat(L"Segoe UI", nullptr, static_cast<DWRITE_FONT_WEIGHT>(c.titleWeight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
        const unsigned textColor = large_icon_render_rules::TextColor(c,
            fill ? view.neutral : background.color, view.componentForeground);
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(D2D1::ColorF(textColor, view.hover * view.opacity), &brush);
        if (format && brush)
        {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.3f, size);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            ComPtr<IDWriteInlineObject> ellipsis;
            fonts->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, ellipsis.Get());
            const float slide = view.animations ? (1 - view.hover) * 6 * view.scale : 0;
            const float x = frame.left + static_cast<float>(geometry.titleLeft) + (geometry.leftReveal ? slide : 0);
            const float y = frame.top + static_cast<float>(geometry.titleTop) + (geometry.upReveal ? slide : 0);
            target->DrawText(view.name.data(), static_cast<UINT32>(view.name.size()), format.Get(),
                D2D1::RectF(x, y, x + static_cast<float>(geometry.titleWidth),
                    y + static_cast<float>(geometry.titleHeight)), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    DrawEdgeGlow(target, c, view, frame, radius);
    if (clip) target->PopLayer();
    else target->PopAxisAlignedClip();
    if (view.selected)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0x75baff, .95f), &brush)))
            target->DrawRoundedRectangle(D2D1::RoundedRect(frame, radius, radius), brush.Get(), view.scale);
    }
}

bool DrawCard3D(ID2D1DeviceContext* target, IDWriteFactory* fonts, const LargeIconConfig& config,
    const View& view, const large_icon_transform::Card& transform, CardResources& resources)
{
    if (!target || !transform.active) return false;
    ComPtr<ID2D1Device> device; target->GetDevice(&device);
    if (device.Get() != resources.device.Get())
    { resources.recorder.Reset(); resources.device = device; }
    if (!device || (!resources.recorder && FAILED(device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &resources.recorder)))) return false;
    auto* recorder = resources.recorder.Get();
    ComPtr<ID2D1CommandList> commands;
    ComPtr<ID2D1Effect> lighting, perspective;
    if (FAILED(recorder->CreateCommandList(&commands)) ||
        FAILED(recorder->CreateEffect(CLSID_D2D1ColorMatrix, &lighting)) ||
        FAILED(recorder->CreateEffect(CLSID_D2D13DTransform, &perspective))) return false;
    float dpiX, dpiY; target->GetDpi(&dpiX, &dpiY);
    recorder->SetDpi(dpiX, dpiY); recorder->SetUnitMode(target->GetUnitMode());
    recorder->SetAntialiasMode(target->GetAntialiasMode());
    recorder->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    recorder->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(view.frame.left), -static_cast<float>(view.frame.top)));
    recorder->SetTarget(commands.Get()); recorder->BeginDraw();
    DrawFrame(recorder, fonts, config, view);
    const HRESULT recorded = recorder->EndDraw();
    recorder->SetTarget(nullptr);
    if (FAILED(recorded) || FAILED(commands->Close())) { resources.recorder.Reset(); return false; }
    lighting->SetInput(0, commands.Get());
    const float light = transform.light;
    const D2D1_MATRIX_5X4_F colors = D2D1::Matrix5x4F(light, 0, 0, 0, 0, light, 0, 0, 0, 0, light, 0, 0, 0, 0, 1, 0, 0, 0, 0);
    if (FAILED(lighting->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, colors)) ||
        FAILED(lighting->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE))) return false;
    perspective->SetInputEffect(0, lighting.Get());
    if (FAILED(perspective->SetValue(D2D1_3DTRANSFORM_PROP_TRANSFORM_MATRIX, transform.matrix))) return false;
    target->DrawImage(perspective.Get(), D2D1::Point2F(static_cast<float>(view.frame.left), static_cast<float>(view.frame.top)));
    return true;
}
}
