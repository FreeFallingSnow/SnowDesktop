#pragma once
#include "large_icon_config.h"
#include "flat_glass_rim.h"
#include <d2d1.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <vector>

namespace snowdesktop::large_icon_shape
{
inline RECT Frame(int shape, RECT frame)
{
    // Keep the grid allocation intact. Only the visible square/circle is
    // centered in it; unequal row/column counts must never create an ellipse.
    if (shape == 1 || shape == 2)
    {
        const LONG edge = std::max<LONG>(0, std::min(frame.right - frame.left, frame.bottom - frame.top));
        frame.left += (frame.right - frame.left - edge) / 2;
        frame.top += (frame.bottom - frame.top - edge) / 2;
        frame.right = frame.left + edge; frame.bottom = frame.top + edge;
    }
    return frame;
}

inline std::vector<D2D1_POINT_2F> Outline(int shape, D2D1_RECT_F frame)
{
    const float w = frame.right - frame.left, h = frame.bottom - frame.top;
    const auto point = [&](float x, float y) { return D2D1::Point2F(frame.left + x * w, frame.top + y * h); };
    switch (shape)
    {
    case 2:
    {
        std::vector<D2D1_POINT_2F> circle;
        constexpr int samples = 128;
        circle.reserve(samples);
        for (int i = 0; i < samples; ++i)
        {
            const double angle = 6.28318530717958647692 * i / samples;
            circle.push_back(point(.5f + .5f * static_cast<float>(std::cos(angle)),
                .5f + .5f * static_cast<float>(std::sin(angle))));
        }
        return circle;
    }
    case 3: return {point(0, 0), point(1, 0), point(.78f, .5f), point(1, 1), point(0, 1)};
    case 4: return {point(.5f, 0), point(1, .5f), point(.5f, 1), point(0, .5f)};
    case 5: return {point(.25f, 0), point(.75f, 0), point(1, .5f), point(.75f, 1), point(.25f, 1), point(0, .5f)};
    default: return {};
    }
}

inline Microsoft::WRL::ComPtr<ID2D1Geometry> Geometry(ID2D1Factory* factory, int shape,
    D2D1_RECT_F frame, float radius)
{
    Microsoft::WRL::ComPtr<ID2D1Geometry> result;
    if (!factory || frame.right <= frame.left || frame.bottom <= frame.top) return result;
    if (shape <= 1)
    {
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> rounded;
        if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame, radius, radius), &rounded))) result = rounded;
    }
    else if (shape == 2)
    {
        Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> circle;
        const float edge = std::min(frame.right - frame.left, frame.bottom - frame.top);
        if (SUCCEEDED(factory->CreateEllipseGeometry(D2D1::Ellipse(
                D2D1::Point2F((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2), edge / 2, edge / 2), &circle))) result = circle;
    }
    else
    {
        const auto outline = Outline(shape, frame);
        if (outline.empty()) return result;
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return result;
        sink->BeginFigure(outline.front(), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(outline.data() + 1, static_cast<UINT32>(outline.size() - 1));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) result = path;
    }
    return result;
}

// Called inside the card's geometry mask. Component materials retain their
// directional edge lighting while their borders follow the selected contour.
inline void DrawMaterialOutline(ID2D1RenderTarget* target, int shape, RECT bounds,
    D2D1_COLOR_F border, float borderWidth, float strength, float edgeWidth,
    const EdgeLightSettings& light)
{
    if (!target || shape <= 1) return;
    const auto frame = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
    Microsoft::WRL::ComPtr<ID2D1Factory> factory; target->GetFactory(&factory);
    const auto geometry = Geometry(factory.Get(), shape, frame, 0);
    if (!geometry) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(border, &brush))) return;
    // The mask retains the inside half of the stroke, matching panel borders.
    if (border.a > 0 && borderWidth > 0) target->DrawGeometry(geometry.Get(), brush.Get(), borderWidth * 2);
    if (strength <= 0 || edgeWidth <= 0) return;
    const flat_glass_rim::Evaluator material(light);
    const auto outline = Outline(shape, frame);
    const float w = frame.right - frame.left, h = frame.bottom - frame.top;
    for (size_t i = 0; i < outline.size(); ++i)
    {
        const auto a = outline[i], b = outline[(i + 1) % outline.size()];
        const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(b.x - a.x, b.y - a.y) / 3.f)));
        const float count = static_cast<float>(steps);
        for (int j = 0; j < steps; ++j)
        {
            const auto sample = [&](float t) { return D2D1::Point2F(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); };
            const auto from = sample(static_cast<float>(j) / count), to = sample(static_cast<float>(j + 1) / count);
            const auto mid = sample((static_cast<float>(j) + .5f) / count);
            const float lighting = material.Lighting((mid.x - frame.left) / w, (mid.y - frame.top) / h);
            brush->SetColor(D2D1::ColorF(0xffffff, std::clamp(strength * lighting, 0.f, 1.f)));
            target->DrawLine(from, to, brush.Get(), material.BorderWidth(edgeWidth, lighting) * 2);
        }
    }
}
}
