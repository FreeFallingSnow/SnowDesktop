#pragma once

#include <d2d1.h>
#include <d2d1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace snowdesktop::smooth_corners
{
// The setting remains an equivalent circular radius: the central 45-degree
// point is unchanged. Two zero-curvature cubic ramps join the straight edges
// to the circular middle with matching tangent and curvature (G2). Room for
// the ramps decreases continuously as the radius approaches a capsule.
struct Corner
{
    float radius = 0, extent = 0, alpha = 0, a = 0, b = 0, c = 0, d = 0;
    Corner(float requested, float limit)
    {
        radius = std::isfinite(requested) ? std::clamp(requested, 0.f, std::max(0.f, limit)) : 0.f;
        extent = std::min(radius * 1.6f, std::max(0.f, limit));
        if (radius <= 0.f) return;
        constexpr float quarterPi = .7853981633974483f;
        alpha = std::max(0.f, extent / radius - 1.f) * quarterPi;
        if (alpha < .0001f) { alpha = 0; extent = radius; return; }
        const float handle = radius * std::tan(alpha * .5f);
        c = handle * std::cos(alpha);
        d = handle * std::sin(alpha);
        // End curvature of the first cubic is 2*b*d/(3*handle^3).
        b = 1.5f * handle * handle * handle / (radius * d);
        const float chord = radius * (std::cos(alpha) - std::sin(alpha));
        a = extent - chord - c - d - b;
    }
    static D2D1_POINT_2F Cubic(D2D1_POINT_2F p0, D2D1_POINT_2F p1,
        D2D1_POINT_2F p2, D2D1_POINT_2F p3, float t)
    {
        const float u = 1.f - t;
        return {u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x,
            u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y};
    }
    D2D1_POINT_2F Point(float t) const
    {
        constexpr float halfPi = 1.5707963267948966f;
        const float end = radius * (1.f - std::sin(alpha));
        if (t < 1.f / 3.f)
            return Cubic({extent, 0}, {extent-a, 0}, {extent-a-b, 0}, {end, d}, t*3);
        if (t > 2.f / 3.f)
            return Cubic({d, end}, {0, extent-a-b}, {0, extent-a}, {0, extent}, t*3-2);
        const float angle = alpha + (halfPi - 2*alpha) * (t*3-1);
        return {radius * (1.f - std::sin(angle)), radius * (1.f - std::cos(angle))};
    }
    float Inset(float axis) const
    {
        if (axis >= extent || radius <= 0.f) return 0.f;
        if (axis <= 0.f) return extent;
        float low = 0.f, high = 1.f;
        for (int i = 0; i < 20; ++i)
        {
            const float middle = (low+high)*.5f;
            if (Point(middle).x > axis) low = middle; else high = middle;
        }
        return Point((low+high)*.5f).y;
    }
};

inline bool Contains(D2D1_RECT_F frame, float radius, D2D1_POINT_2F point)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < frame.left ||
        point.y < frame.top || point.x >= frame.right || point.y >= frame.bottom) return false;
    const float x = std::min(point.x-frame.left, frame.right-point.x);
    const float y = std::min(point.y-frame.top, frame.bottom-point.y);
    const Corner corner(radius, std::min(frame.right-frame.left, frame.bottom-frame.top)*.5f);
    return y >= corner.Inset(x);
}

template<class Sink> void Trace(Sink* sink, D2D1_RECT_F frame, float radius)
{
    const Corner corner(radius, std::min(frame.right-frame.left, frame.bottom-frame.top) * .5f);
    const auto map = [&](int side, float x, float y) -> D2D1_POINT_2F {
        switch (side)
        {
        case 0: return {frame.right-x, frame.top+y};
        case 1: return {frame.right-y, frame.bottom-x};
        case 2: return {frame.left+x, frame.bottom-y};
        default: return {frame.left+y, frame.top+x};
        }
    };
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(map(0, corner.extent, 0), D2D1_FIGURE_BEGIN_FILLED);
    for (int side = 0; side < 4; ++side)
    {
        const float end = corner.radius * (1.f - std::sin(corner.alpha));
        if (corner.alpha > 0.f)
            sink->AddBezier(D2D1::BezierSegment(map(side, corner.extent-corner.a, 0),
                map(side, corner.extent-corner.a-corner.b, 0), map(side, end, corner.d)));
        if (corner.radius > 0.f)
            sink->AddArc(D2D1::ArcSegment(map(side, corner.d, end),
                D2D1::SizeF(corner.radius, corner.radius), 0,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        if (corner.alpha > 0.f)
            sink->AddBezier(D2D1::BezierSegment(map(side, 0, corner.extent-corner.a-corner.b),
                map(side, 0, corner.extent-corner.a), map(side, 0, corner.extent)));
        sink->AddLine(map((side+1)%4, corner.extent, 0));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

inline Microsoft::WRL::ComPtr<ID2D1PathGeometry> CreateGeometry(
    ID2D1Factory* factory, D2D1_RECT_F frame, float radius)
{
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (!factory || !(frame.right > frame.left) || !(frame.bottom > frame.top)) return path;
    if (FAILED(factory->CreatePathGeometry(&path))) return {};
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return {};
    Trace(sink.Get(), frame, radius);
    if (FAILED(sink->Close())) return {};
    return path;
}

// Bounded per-render-thread cache. Owning the factory prevents stale pointer
// reuse after device recovery; a new factory drops all geometry from the old.
inline Microsoft::WRL::ComPtr<ID2D1PathGeometry> Geometry(
    ID2D1Factory* factory, D2D1_RECT_F frame, float radius)
{
    struct Entry { D2D1_RECT_F frame; float radius; Microsoft::WRL::ComPtr<ID2D1PathGeometry> path; };
    thread_local Microsoft::WRL::ComPtr<ID2D1Factory> owner;
    thread_local std::vector<Entry> entries;
    thread_local std::size_t next = 0;
    if (owner.Get() != factory) { entries.clear(); owner = factory; next = 0; }
    for (const auto& entry : entries)
        if (entry.radius == radius && entry.frame.left == frame.left && entry.frame.top == frame.top &&
            entry.frame.right == frame.right && entry.frame.bottom == frame.bottom) return entry.path;
    auto path = CreateGeometry(factory, frame, radius);
    if (path)
    {
        Entry entry{frame, radius, path};
        if (entries.size() < 128) entries.push_back(std::move(entry));
        else { entries[next] = std::move(entry); next = (next+1)%128; }
    }
    return path;
}

inline void Fill(ID2D1RenderTarget* target, D2D1_RECT_F frame, float radius, ID2D1Brush* brush)
{
    if (!target || !brush) return;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory; target->GetFactory(&factory);
    if (const auto path = Geometry(factory.Get(), frame, radius)) target->FillGeometry(path.Get(), brush);
}
inline void Stroke(ID2D1RenderTarget* target, D2D1_RECT_F frame, float radius, ID2D1Brush* brush, float width)
{
    if (!target || !brush) return;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory; target->GetFactory(&factory);
    if (const auto path = Geometry(factory.Get(), frame, radius)) target->DrawGeometry(path.Get(), brush, width);
}

// A subpixel polyline of the same contour is built once per cached rim mask.
// Most samples use the constant-time straight-edge branch; only corner pixels
// examine segments. Negative distance denotes the inside of the panel.
class Distance
{
    Corner corner_;
    float width_, height_;
    std::array<D2D1_POINT_2F, 49> points_{};
public:
    Distance(float width, float height, float radius) :
        corner_(radius, std::min(width, height)*.5f), width_(width), height_(height)
    {
        for (std::size_t i = 0; i < points_.size(); ++i)
            points_[i] = corner_.Point(static_cast<float>(i) / static_cast<float>(points_.size()-1));
    }
    float At(float px, float py) const
    {
        const float x = std::min(px, width_-px), y = std::min(py, height_-py);
        if (x >= corner_.extent || y >= corner_.extent) return -std::min(x, y);
        if (corner_.radius <= 0.f) return std::hypot(std::min(x, 0.f), std::min(y, 0.f));
        float squared = (std::numeric_limits<float>::max)();
        bool inside = false;
        for (std::size_t i = 1; i < points_.size(); ++i)
        {
            const auto p = points_[i-1], q = points_[i];
            const float dx = q.x-p.x, dy = q.y-p.y, length = dx*dx+dy*dy;
            const float t = length > 0.f ? std::clamp(((x-p.x)*dx+(y-p.y)*dy)/length, 0.f, 1.f) : 0.f;
            const float ox = x-p.x-t*dx, oy = y-p.y-t*dy;
            squared = std::min(squared, ox*ox+oy*oy);
            if (x >= q.x && x <= p.x && dx < 0.f)
                inside = y >= p.y+(x-p.x)*dy/dx;
        }
        return std::sqrt(squared) * (inside ? -1.f : 1.f);
    }
};
}
