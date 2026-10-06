#pragma once
#include "panel_gradient.h"
#include <d2d1.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <array>

namespace snowdesktop
{
inline bool DrawPanelGradient(ID2D1RenderTarget* target, D2D1_RECT_F frame,
    float radius, const PanelGradient& gradient, float opacity = 1)
{
    if (!target || !gradient.enabled || !ValidatePanelGradient(gradient)) return false;
    std::array<D2D1_GRADIENT_STOP, 5> colors{};
    for (size_t i = 0; i < gradient.stops.size(); ++i)
        colors[i] = {static_cast<float>(gradient.stops[i].position),
            D2D1::ColorF(gradient.stops[i].color, static_cast<float>(gradient.stops[i].opacity) * opacity)};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
    if (FAILED(target->CreateGradientStopCollection(colors.data(), static_cast<UINT32>(gradient.stops.size()),
            D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops))) return false;
    const auto line = ResolvePanelGradientLine(gradient, frame.right - frame.left, frame.bottom - frame.top);
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
    if (FAILED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(frame.left + static_cast<float>(line.x1), frame.top + static_cast<float>(line.y1)),
            D2D1::Point2F(frame.left + static_cast<float>(line.x2), frame.top + static_cast<float>(line.y2))), stops.Get(), &brush))) return false;
    target->FillRoundedRectangle(D2D1::RoundedRect(frame, radius, radius), brush.Get());
    return true;
}
}
