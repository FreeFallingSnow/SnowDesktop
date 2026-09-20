#pragma once

#include <d2d1.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <algorithm>

namespace snowdesktop::icon_loading_placeholder
{
// A Shell-independent, render-only loading mark. No PIDL, image-list index,
// filesystem access, timer or interactive item is needed before enumeration.
inline void Draw(ID2D1RenderTarget* target, RECT bounds, float opacity = 1.0f)
{
    if (!target || bounds.right <= bounds.left || bounds.bottom <= bounds.top || opacity <= 0)
        return;
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    const float size = static_cast<float>(std::min(
        bounds.right - bounds.left, bounds.bottom - bounds.top));
    const float x = (bounds.left + bounds.right) * 0.5f;
    const float y = (bounds.top + bounds.bottom) * 0.5f;
    const float half = size * 0.40f;
    const auto plate = D2D1::RoundedRect(
        D2D1::RectF(x - half, y - half, x + half, y + half),
        size * 0.16f, size * 0.16f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.45f, 0.48f, 0.53f, 0.70f * opacity), &brush)))
        return;
    target->FillRoundedRectangle(plate, brush.Get());
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f * opacity));
    for (int dot = -1; dot <= 1; ++dot)
        target->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(x + dot * size * 0.18f, y),
            size * 0.055f, size * 0.055f), brush.Get());
}
}
