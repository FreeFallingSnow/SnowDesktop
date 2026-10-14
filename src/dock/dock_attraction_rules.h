/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0.
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Adapted from OpenHarmony RSAttractionEffectFilter, commit
 * e72069008cd4762ddd150d164861c43f8183e439. SnowDesktop supplies the
 * Coons sampling, cropped-image projection, bounds and endpoint handling.
 */
#pragma once

#include "dock_genie_rules.h"

#include <array>
#include <optional>

namespace snowdesktop::dock_attraction
{
using Rect = dock_genie::Rect;
using Matrix = dock_genie::ProjectiveMatrix;
struct Point
{
    double x = 0, y = 0;
    Point operator+(Point p) const noexcept
    {
        return {x + p.x, y + p.y};
    }
    Point operator-(Point p) const noexcept
    {
        return {x - p.x, y - p.y};
    }
    Point operator*(double amount) const noexcept
    {
        return {x * amount, y * amount};
    }
};
using Patch = std::array<Point, 12>;
inline constexpr std::size_t GridSize = 16;
inline constexpr std::size_t CellCount = GridSize * GridSize;

inline Point Bezier(Point a, Point b, Point c, Point d, double t) noexcept
{
    const double u = 1 - t;
    return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t);
}
inline double Timing(double fraction, double firstX, double secondX, double secondY) noexcept
{
    double low = 0, high = 1;
    for (int i = 0; i < 30; ++i)
    {
        const double mid = (low + high) * 0.5;
        if (Bezier({0, 0}, {firstX, 0}, {secondX, 1}, {1, 1}, mid).x < fraction)
            low = mid;
        else
            high = mid;
    }
    const double t = (low + high) * 0.5;
    return Bezier({0, 0}, {firstX, 0}, {secondX, secondY}, {1, 1}, t).y;
}
inline Patch Boundary(const std::array<Point, 4> &corners) noexcept
{
    const auto [tl, tr, bl, br] = corners;
    return {tl, tl * 0.67 + tr * 0.33, tl * 0.33 + tr * 0.67, tr, tr * 0.67 + br * 0.33, tr * 0.33 + br * 0.67,
            br, bl * 0.33 + br * 0.67, bl * 0.67 + br * 0.33, bl, tl * 0.33 + bl * 0.67, tl * 0.67 + bl * 0.33};
}
inline Patch WindowBoundary(double width, double height) noexcept
{
    return {{{0, 0},
             {width / 3, 0},
             {width * 2 / 3, 0},
             {width, 0},
             {width, height / 3},
             {width, height * 2 / 3},
             {width, height},
             {width * 2 / 3, height},
             {width / 3, height},
             {0, height},
             {0, height * 2 / 3},
             {0, height / 3}}};
}
inline Patch ControlPoints(Rect window, Rect dock, double collapsed) noexcept
{
    const double w = window.right - window.left, h = window.bottom - window.top;
    const Point target{(dock.left + dock.right) * 0.5 - window.left, (dock.top + dock.bottom) * 0.5 - window.top};
    const Patch initial = WindowBoundary(w, h);
    if (collapsed <= 0)
    {
        Patch result = initial;
        for (auto &p : result)
            p = p + Point{window.left, window.top};
        return result;
    }
    const bool below = h * 0.5 > target.y;
    const double location = w * 0.5 > target.x ? 1 : -1;
    const double dx = h > target.y ? std::abs(h * 0.5 - target.y) : std::abs(w * 0.5 - target.x);
    const double dy = h > target.y ? std::abs((location == 1 ? 0 : w) - target.x) : std::abs(h - target.y);
    const double width = below ? h : w, height = below ? w : h;
    const std::array<Point, 4> upper{
        {{((below ? 0.016 : -0.100) * width - (below ? 0.08 : 0.008) * dx) * location,
          (below ? 0.464 : 0.008) * height + (below ? 0.40 : 0.085) * dy},
         {(-1.147 * width - 0.016 * dx) * location, -0.187 * height + 0.30 * dy},
         {((below ? -0.15 : -0.008) * width - (below ? 0.075 : 0.008) * dx) * location, (below ? 0.2 : -0.008) * dy},
         {(-0.848 * width - 0.2 * dx) * location, -0.859 * height - 0.2 * dy}}};
    const double cross = dx * height / std::max(1.0, width) - width;
    const std::array<Point, 4> lower{{{(below ? 0.300 : 0.131) * cross * location, -0.20 * dy},
                                      {(below ? 0.450 : 0.147) * cross * location, -0.30 * dy},
                                      {(below ? 0.150 : 0.085) * cross * location, (below ? -0.20 : 0.008) * dy},
                                      {(below ? 0.300 : 0.147) * cross * location, (below ? -0.112 : -0.069) * dy}}};
    const auto first = Boundary(upper), second = Boundary(lower);
    const std::array<int, 12> indices =
        !below ? (location == 1 ? std::array<int, 12>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}
                                : std::array<int, 12>{3, 2, 1, 0, 11, 10, 9, 8, 7, 6, 5, 4})
               : (location == 1 ? std::array<int, 12>{9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 7, 8}
                                : std::array<int, 12>{0, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1});
    const double bottomX = below ? 0.2 : 0;
    const std::array<double, 12> speed{(0.5 + bottomX) * 0.5,
                                       bottomX * 0.33 + 0.5 * 0.67,
                                       bottomX * 0.17 + 0.5 * 0.83,
                                       0.5,
                                       bottomX * 0.17 + 0.5 * (below ? 0.68 : 0.83),
                                       bottomX * 0.33 + 0.5 * (below ? 0.57 : 0.67),
                                       (0.5 + bottomX) * 0.5,
                                       bottomX * 0.67 + 0.5 * 0.33,
                                       bottomX * 0.83 + 0.5 * 0.17,
                                       bottomX,
                                       bottomX * 0.83 + 0.5 * 0.17,
                                       bottomX * 0.67 + 0.5 * 0.33};
    Patch result{};
    const auto control = [&](const Patch &path, std::size_t i, Point offset) {
        if (!below)
            return path[i] + offset;
        if (location == 1)
            return Point{-path[i].y + offset.x, path[i].x + offset.y};
        return Point{path[i].y + offset.x, -path[i].x + offset.y};
    };
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const auto j = static_cast<std::size_t>(indices[i]);
        const double time = Timing(std::clamp(collapsed, 0.0, 1.0), speed[j], speed[j],
                                   below && j == 4   ? 0.85
                                   : below && j == 5 ? 0.90
                                                     : 1.0);
        result[i] = Bezier(initial[i], control(first, j, initial[i]), control(second, j, target), target, time);
        // The point endpoint has zero area. Retain a subpixel source-shaped
        // core for finite compositor matrices; the endpoint is transparent.
        result[i] = result[i] + (initial[i] - Point{w * 0.5, h * 0.5}) * (0.002 * std::pow(collapsed, 8));
        result[i] = result[i] + Point{window.left, window.top};
    }
    return result;
}
inline Point Surface(const Patch &p, double u, double v) noexcept
{
    const auto top = Bezier(p[0], p[1], p[2], p[3], u);
    const auto bottom = Bezier(p[9], p[8], p[7], p[6], u);
    const auto left = Bezier(p[0], p[11], p[10], p[9], v);
    const auto right = Bezier(p[3], p[4], p[5], p[6], v);
    const auto corners = p[0] * ((1 - u) * (1 - v)) + p[3] * (u * (1 - v)) + p[9] * ((1 - u) * v) + p[6] * (u * v);
    return top * (1 - v) + bottom * v + left * (1 - u) + right * u - corners;
}
struct Cell
{
    double left, top, right, bottom;
};
inline Cell SourceCell(std::size_t index) noexcept
{
    const auto x = index % GridSize, y = index / GridSize;
    return {static_cast<double>(x) / GridSize, static_cast<double>(y) / GridSize, static_cast<double>(x + 1) / GridSize,
            static_cast<double>(y + 1) / GridSize};
}
inline std::optional<Matrix> Projection(const Patch &patch, Cell cell, double sourceWidth, double sourceHeight,
                                        double cropX, double cropY, double hostX, double hostY) noexcept
{
    const Point offset{hostX, hostY};
    const Point a = Surface(patch, cell.left, cell.top) - offset;
    const Point b = Surface(patch, cell.right, cell.top) - offset;
    const Point c = Surface(patch, cell.right, cell.bottom) - offset;
    const Point d = Surface(patch, cell.left, cell.bottom) - offset;
    const Point e = b - c, f = d - c, delta = a - b + c - d;
    const double determinant = e.x * f.y - f.x * e.y;
    if (std::abs(determinant) < 1e-12)
        return std::nullopt;
    const double g = (delta.x * f.y - f.x * delta.y) / determinant;
    const double h = (e.x * delta.y - delta.x * e.y) / determinant;
    // A local fold is a zero-area patch boundary, not an unbounded projective
    // image. Hide only that cell instead of introducing a perspective pole.
    const double w = sourceWidth * (cell.right - cell.left), height = sourceHeight * (cell.bottom - cell.top);
    double smallest = 1, largest = 1;
    for (const double u : {-0.5 / w, 1 + 0.5 / w})
        for (const double v : {-0.5 / height, 1 + 0.5 / height})
        {
            const double den = 1 + g * u + h * v;
            smallest = std::min(smallest, den);
            largest = std::max(largest, den);
        }
    if (smallest / largest < 0.005)
        return std::nullopt;
    const double x = sourceWidth * cell.left - cropX, y = sourceHeight * cell.top - cropY;
    const double scale = 1.0 / std::max({1.0, 1 + g, 1 + h, 1 + g + h});
    Matrix m;
    m.m11 = (b.x - a.x + g * b.x) / w * scale;
    m.m12 = (b.y - a.y + g * b.y) / w * scale;
    m.m21 = (d.x - a.x + h * d.x) / height * scale;
    m.m22 = (d.y - a.y + h * d.y) / height * scale;
    m.m14 = g / w * scale;
    m.m24 = h / height * scale;
    m.dx = a.x * scale - m.m11 * x - m.m21 * y;
    m.dy = a.y * scale - m.m12 * x - m.m22 * y;
    m.w = scale - m.m14 * x - m.m24 * y;
    return m;
}
inline double Opacity(double collapsed) noexcept
{
    return 1 - dock_genie::Smooth((collapsed - 0.88) / 0.12);
}
inline double CellOpacity(Rect window, Rect dock, double collapsed, Cell cell) noexcept
{
    const bool right = window.left + window.right > dock.left + dock.right;
    const bool below = window.top + window.bottom > dock.top + dock.bottom;
    const double u = (cell.left + cell.right) * 0.5, v = (cell.top + cell.bottom) * 0.5;
    const double w = window.right - window.left, h = window.bottom - window.top;
    const double gradient = below ? ((right ? u : 1 - u) * w * w + v * h * h) / (w * w + h * h) : 1 - v;
    return 1 - std::min(1.0, collapsed * 2) * gradient;
}
inline Rect Bounds(Rect window, Rect dock) noexcept
{
    // A Coons patch is contained by its equivalent bicubic control hull.
    // Prepare the whole motion's envelope, with a guard for extrema between
    // samples; reversing must never use the rectangular progress proxy.
    Rect result{std::min(window.left, dock.left), std::min(window.top, dock.top), std::max(window.right, dock.right),
                std::max(window.bottom, dock.bottom)};
    for (int phase = 0; phase <= 128; ++phase)
    {
        const auto patch = ControlPoints(window, dock, phase / 128.0);
        // A Coons surface is a convex combination of bicubic controls. The
        // interior controls are derived from the four cubic boundaries.
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
            {
                const double u = x / 3.0, v = y / 3.0;
                const std::array<Point, 4> top{patch[0], patch[1], patch[2], patch[3]};
                const std::array<Point, 4> bottom{patch[9], patch[8], patch[7], patch[6]};
                const std::array<Point, 4> left{patch[0], patch[11], patch[10], patch[9]};
                const std::array<Point, 4> right{patch[3], patch[4], patch[5], patch[6]};
                const Point p = top[x] * (1 - v) + bottom[x] * v + left[y] * (1 - u) + right[y] * u -
                                (patch[0] * ((1 - u) * (1 - v)) + patch[3] * (u * (1 - v)) + patch[9] * ((1 - u) * v) +
                                 patch[6] * (u * v));
                result.left = std::min(result.left, p.x);
                result.top = std::min(result.top, p.y);
                result.right = std::max(result.right, p.x);
                result.bottom = std::max(result.bottom, p.y);
            }
    }
    constexpr double margin = 4;
    return {std::floor(result.left - margin), std::floor(result.top - margin), std::ceil(result.right + margin),
            std::ceil(result.bottom + margin)};
}
} // namespace snowdesktop::dock_attraction
