#pragma once

#include "icon_beautify.h"
#include "ui/render/icon_backdrop_clip.h"

#include <d2d1helper.h>
#include <algorithm>

namespace snowdesktop
{

inline D2D1_POINT_2F IconOutlinePoint(const icon_beautify::ShapePoint& point,
    float width, float height, bool preserveCorners) noexcept
{
    if (!preserveCorners) return D2D1::Point2F(point.x * width, point.y * height);
    const float side = (std::min)(width, height);
    return D2D1::Point2F(
        point.x <= .5f ? point.x * side : width - (1.f - point.x) * side,
        point.y <= .5f ? point.y * side : height - (1.f - point.y) * side);
}

// Keep the original icon silhouette and position. Cropping its frame before
// constructing the silhouette would squash a partly visible circle or shield.
inline HRESULT CreateIconBackdropGeometry(ID2D1Factory* fallbackFactory,
    IconBeautifyShape shape, const RECT& frame, const IconBackdropClip* clip,
    Microsoft::WRL::ComPtr<ID2D1Geometry>& result)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID2D1Factory> factory = fallbackFactory;
    if (clip && clip->geometry)
        clip->geometry->geometry->GetFactory(factory.ReleaseAndGetAddressOf());
    if (!factory) return E_POINTER;

    ComPtr<ID2D1PathGeometry> path;
    HRESULT hr = factory->CreatePathGeometry(&path);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1GeometrySink> sink;
    hr = path->Open(&sink);
    if (FAILED(hr)) return hr;
    const auto& outline = icon_beautify::ShapeOutline(shape);
    const auto point = [&](const auto& p) {
        return IconOutlinePoint(p, static_cast<float>(frame.right - frame.left),
            static_cast<float>(frame.bottom - frame.top), clip && clip->preserveCorners);
    };
    sink->BeginFigure(point(outline.front()), D2D1_FIGURE_BEGIN_FILLED);
    for (std::size_t i = 1; i < outline.size(); ++i) sink->AddLine(point(outline[i]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1Geometry> geometry = path;
    const auto intersect = [&](ID2D1Geometry* mask,
        const D2D1_MATRIX_3X2_F& transform) -> HRESULT {
        ComPtr<ID2D1PathGeometry> intersection;
        HRESULT status = factory->CreatePathGeometry(&intersection);
        if (FAILED(status)) return status;
        ComPtr<ID2D1GeometrySink> output;
        status = intersection->Open(&output);
        if (FAILED(status)) return status;
        status = geometry->CombineWithGeometry(mask, D2D1_COMBINE_MODE_INTERSECT,
            transform, output.Get());
        const HRESULT close = output->Close();
        if (FAILED(status)) return status;
        if (FAILED(close)) return close;
        geometry = intersection;
        return S_OK;
    };
    if (clip)
    {
        ComPtr<ID2D1RectangleGeometry> viewport;
        hr = factory->CreateRectangleGeometry(D2D1::RectF(
            static_cast<float>(clip->bounds.left - frame.left),
            static_cast<float>(clip->bounds.top - frame.top),
            static_cast<float>(clip->bounds.right - frame.left),
            static_cast<float>(clip->bounds.bottom - frame.top)), &viewport);
        if (FAILED(hr)) return hr;
        hr = intersect(viewport.Get(), D2D1::Matrix3x2F::Identity());
        if (FAILED(hr)) return hr;
        for (auto mask = clip->geometry; mask; mask = mask->parent)
        {
            hr = intersect(mask->geometry.Get(), D2D1::Matrix3x2F::Translation(
                static_cast<float>(clip->geometryOffset.x - frame.left),
                static_cast<float>(clip->geometryOffset.y - frame.top)));
            if (FAILED(hr)) return hr;
        }
    }
    result = std::move(geometry);
    return S_OK;
}

} // namespace snowdesktop
