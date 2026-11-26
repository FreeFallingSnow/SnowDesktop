#pragma once

#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <initializer_list>

namespace snowdesktop
{
inline void DrawPlacementPreview(ID2D1RenderTarget* target, ID2D1Geometry* outline,
    float scale, bool valid, const RECT* gridAllocation)
{
    if (!target || !outline) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    const unsigned color = valid ? 0x68b5ff : 0xf16d70;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(color, .2f), &brush)))
        return;

    if (gridAllocation && !IsRectEmpty(gridAllocation))
    {
        // Brackets show the occupied grid without filling the margins around the shape.
        const auto grid = D2D1::RectF(
            static_cast<float>(gridAllocation->left), static_cast<float>(gridAllocation->top),
            static_cast<float>(gridAllocation->right), static_cast<float>(gridAllocation->bottom));
        const float length = std::min(16.f * std::max(1.f, scale),
            std::min(grid.right - grid.left, grid.bottom - grid.top) * .25f);
        brush->SetColor(D2D1::ColorF(color, .65f));
        for (const float x : {grid.left, grid.right})
            for (const float y : {grid.top, grid.bottom})
            {
                const auto corner = D2D1::Point2F(x, y);
                target->DrawLine(corner,
                    D2D1::Point2F(x + (x == grid.left ? length : -length), y), brush.Get(), 2.f);
                target->DrawLine(corner,
                    D2D1::Point2F(x, y + (y == grid.top ? length : -length)), brush.Get(), 2.f);
            }
        brush->SetColor(D2D1::ColorF(color, .2f));
    }

    target->FillGeometry(outline, brush.Get());
    brush->SetColor(D2D1::ColorF(color, .95f));
    target->DrawGeometry(outline, brush.Get(), 2.f);
}
}
