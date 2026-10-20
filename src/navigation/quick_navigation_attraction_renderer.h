#pragma once

#include "quick_navigation_attraction_rules.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <wrl/client.h>

namespace snowdesktop::quick_navigation_animation_rules
{
// All triangles are rasterized into one premultiplied image. In particular,
// do not implement joins with independently transformed DComp rectangle clips:
// their rasterized boundaries can disagree, exposing bright backdrop pixels.
inline HRESULT DrawAttractionMesh(ID2D1DeviceContext* context, ID2D1Bitmap1* source,
    const dock_attraction::Frame& frame, double hostX, double hostY,
    double panelWidth, double panelHeight)
{
    if (!context || !source || panelWidth <= 0 || panelHeight <= 0)
        return E_INVALIDARG;
    namespace attraction = dock_attraction;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID2D1BitmapBrush1> brush;
    HRESULT hr = context->CreateBitmapBrush(source,
        D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
            D2D1_INTERPOLATION_MODE_LINEAR), &brush);
    if (FAILED(hr)) return hr;

    // Convert each shared vertex once. FillMesh uses the same aliased edge
    // ownership on both sides, without expanding or independently AA'ing joins.
    std::array<D2D1_POINT_2F, attraction::VertexCount> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i)
        vertices[i] = D2D1::Point2F(static_cast<float>(frame.vertices[i].x - hostX),
            static_cast<float>(frame.vertices[i].y - hostY));
    const auto previousAA = context->GetAntialiasMode();
    const auto previousBlend = context->GetPrimitiveBlend();
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    // A folded part still represents one UI image, rather than extra layers of
    // tint/noise. Copy its premultiplied texel, then apply opacity to the image.
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    for (std::size_t i = 0; i < attraction::TriangleCount && SUCCEEDED(hr); ++i)
    {
        const auto indices = attraction::TriangleVertices(i);
        const auto a = vertices[indices[0]], b = vertices[indices[1]], c = vertices[indices[2]];
        const double sign = i % 2 == 0 ? 1.0 : -1.0;
        const double cellWidth = panelWidth / attraction::GridSize;
        const double cellHeight = panelHeight / attraction::GridSize;
        const auto cell = attraction::SourceCell(i / 2);
        const double sourceX = (i % 2 == 0 ? cell.left : cell.right) * panelWidth;
        const double sourceY = (i % 2 == 0 ? cell.top : cell.bottom) * panelHeight;
        const double xx = (b.x - a.x) * sign / cellWidth;
        const double xy = (b.y - a.y) * sign / cellWidth;
        const double yx = (c.x - a.x) * sign / cellHeight;
        const double yy = (c.y - a.y) * sign / cellHeight;
        const D2D1_MATRIX_3X2_F matrix{static_cast<float>(xx), static_cast<float>(xy),
            static_cast<float>(yx), static_cast<float>(yy),
            static_cast<float>(a.x - sourceX * xx - sourceY * yx),
            static_cast<float>(a.y - sourceX * xy - sourceY * yy)};
        // Zero-area triangles cover no pixels and cannot supply an invertible
        // bitmap-brush transform (folds can pass through exactly this state).
        if (std::abs(xx * yy - xy * yx) < 1e-12) continue;
        brush->SetTransform(matrix);
        ComPtr<ID2D1Mesh> mesh;
        ComPtr<ID2D1TessellationSink> sink;
        hr = context->CreateMesh(&mesh);
        if (SUCCEEDED(hr)) hr = mesh->Open(&sink);
        if (SUCCEEDED(hr))
        {
            const D2D1_TRIANGLE triangle{a, b, c};
            sink->AddTriangles(&triangle, 1);
            hr = sink->Close();
        }
        if (SUCCEEDED(hr)) context->FillMesh(mesh.Get(), brush.Get());
    }
    context->SetPrimitiveBlend(previousBlend);
    context->SetAntialiasMode(previousAA);
    return hr;
}
}
