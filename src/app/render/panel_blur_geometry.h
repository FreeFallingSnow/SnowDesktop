#pragma once

#include "desktop_backdrop_compositor.h"
#include "icons/icon_backdrop_geometry.h"
#include "ui/render/smooth_corner_geometry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace snowdesktop::panel_blur
{
using Microsoft::WRL::ComPtr;

inline int RadiusKey(float radius) noexcept
{
    return static_cast<int>(std::lround(std::clamp(radius, 0.0f, 48.0f)));
}

inline bool SameIcon(const PanelBackdropIcon& a, const PanelBackdropIcon& b)
{
    return EqualRect(&a.frame, &b.frame) && a.shape == b.shape &&
        RadiusKey(a.blurRadius) == RadiusKey(b.blurRadius) && SameIconBackdropClip(a.clip, b.clip);
}

inline bool Covered(const RECT& panel, float cornerRadius, const RECT& rect)
{
    if (rect.left < panel.left || rect.top < panel.top ||
        rect.right > panel.right || rect.bottom > panel.bottom) return false;
    const LONG extent = static_cast<LONG>(std::ceil(
        std::min(cornerRadius * 1.6f,
            static_cast<float>(std::min(panel.right - panel.left, panel.bottom - panel.top)) * 0.5f)));
    return (rect.left >= panel.left + extent && rect.right <= panel.right - extent) ||
        (rect.top >= panel.top + extent && rect.bottom <= panel.bottom - extent);
}

struct Geometry
{
    RECT allocation{};
    ComPtr<ID2D1Geometry> outline;
    std::map<int, ComPtr<ID2D1Geometry>> strongerMasks;
};

inline HRESULT Union(ID2D1Factory& factory, ComPtr<ID2D1Geometry>& destination,
    ID2D1Geometry& source)
{
    if (!destination)
    {
        destination = &source;
        return S_OK;
    }
    ComPtr<ID2D1PathGeometry> path;
    HRESULT status = factory.CreatePathGeometry(&path);
    if (FAILED(status)) return status;
    ComPtr<ID2D1GeometrySink> sink;
    status = path->Open(&sink);
    if (FAILED(status)) return status;
    status = destination->CombineWithGeometry(&source, D2D1_COMBINE_MODE_UNION,
        nullptr, sink.Get());
    const HRESULT close = sink->Close();
    if (FAILED(status)) return status;
    if (FAILED(close)) return close;
    destination = std::move(path);
    return S_OK;
}

inline HRESULT Build(ID2D1Factory& factory, const RECT& panel,
    float cornerRadius, int panelRadius, std::span<const PanelBackdropIcon> icons,
    bool extendOutline, Geometry& result)
{
    Geometry candidate;
    candidate.allocation = panel;
    if (extendOutline)
    {
        for (const auto& icon : icons)
        {
            RECT visible = icon.frame;
            if (icon.clip && !IntersectRect(&visible, &icon.frame, &icon.clip->bounds)) continue;
            UnionRect(&candidate.allocation, &candidate.allocation, &visible);
        }
        const auto& area = candidate.allocation;
        candidate.outline = smooth_corners::Geometry(&factory,
            D2D1::RectF(static_cast<float>(panel.left - area.left),
                static_cast<float>(panel.top - area.top),
                static_cast<float>(panel.right - area.left),
                static_cast<float>(panel.bottom - area.top)), cornerRadius);
        if (!candidate.outline) return E_FAIL;
    }

    // Combine in brush coordinates. Paint stronger masks in increasing radius
    // order so the largest radius wins wherever silhouettes overlap.
    for (const auto& icon : icons)
    {
        const int radius = RadiusKey(icon.blurRadius);
        const bool extends = extendOutline && !Covered(panel, cornerRadius, icon.frame);
        if (!extends && radius <= panelRadius) continue;
        RECT visible{};
        if (IsRectEmpty(&icon.frame) ||
            (icon.clip && !IntersectRect(&visible, &icon.frame, &icon.clip->bounds))) continue;

        ComPtr<ID2D1Geometry> silhouette;
        HRESULT status = CreateIconBackdropGeometry(&factory, icon.shape, icon.frame,
            icon.clip ? &*icon.clip : nullptr, silhouette);
        if (FAILED(status)) return status;
        // A captured clip can come from a different factory. Write the path to
        // the graphics device's factory before drawing or combining it.
        ComPtr<ID2D1PathGeometry> local;
        status = factory.CreatePathGeometry(&local);
        if (FAILED(status)) return status;
        ComPtr<ID2D1GeometrySink> sink;
        status = local->Open(&sink);
        if (FAILED(status)) return status;
        status = silhouette->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES,
            D2D1::Matrix3x2F::Translation(static_cast<float>(icon.frame.left - candidate.allocation.left),
                static_cast<float>(icon.frame.top - candidate.allocation.top)), sink.Get());
        const HRESULT close = sink->Close();
        if (FAILED(status)) return status;
        if (FAILED(close)) return close;
        if (extends)
        {
            status = Union(factory, candidate.outline, *local.Get());
            if (FAILED(status)) return status;
        }
        if (radius > panelRadius)
        {
            status = Union(factory, candidate.strongerMasks[radius], *local.Get());
            if (FAILED(status)) return status;
        }
    }
    result = std::move(candidate);
    return S_OK;
}
} // namespace snowdesktop::panel_blur
