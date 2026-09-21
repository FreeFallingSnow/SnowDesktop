#pragma once

#include <d2d1.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include "icon_beautify.h"

namespace snowdesktop::icon_loading_placeholder
{
// A Shell-independent, render-only loading mark. No PIDL, image-list index,
// filesystem access, timer or interactive item is needed before enumeration.
inline void Draw(ID2D1RenderTarget* target, RECT bounds, float opacity = 1.0f,
    IconBeautifyShape shape = IconBeautifyShape::LegacyRounded)
{
    if (!target || bounds.right <= bounds.left || bounds.bottom <= bounds.top || opacity <= 0)
        return;
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    const float size = static_cast<float>(std::min(
        bounds.right - bounds.left, bounds.bottom - bounds.top));
    const float x = (bounds.left + bounds.right) * 0.5f;
    const float y = (bounds.top + bounds.bottom) * 0.5f;
    // Use the full icon footprint and the same outline as the finished plate;
    // contentScale belongs to the artwork inside it, not to its outer bounds.
    const auto& outline = icon_beautify::ShapeOutline(shape);
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(&factory);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> plate;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (!factory || outline.empty() || FAILED(factory->CreatePathGeometry(&plate)) ||
        FAILED(plate->Open(&sink))) return;
    const auto point = [&](const icon_beautify::ShapePoint& p) {
        return D2D1::Point2F(x + (p.x - 0.5f) * size, y + (p.y - 0.5f) * size);
    };
    sink->BeginFigure(point(outline.front()), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t index = 1; index < outline.size(); ++index)
        sink->AddLine(point(outline[index]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.45f, 0.48f, 0.53f, 0.70f * opacity), &brush)))
        return;
    target->FillGeometry(plate.Get(), brush.Get());
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f * opacity));
    for (int dot = -1; dot <= 1; ++dot)
        target->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(x + dot * size * 0.18f, y),
            size * 0.055f, size * 0.055f), brush.Get());
}
}
