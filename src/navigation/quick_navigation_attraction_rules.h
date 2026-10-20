#pragma once

#include "dock/dock_attraction_timeline.h"

namespace snowdesktop::quick_navigation_animation_rules
{
// Map the rounded glass silhouette through the same piecewise affine mesh as
// the content, including folds. Sampling the curved patch independently would
// make the two layers disagree between vertices.
inline dock_attraction::Point AttractionMeshPoint(const dock_attraction::Frame& frame,
    double u, double v) noexcept
{
    namespace attraction = dock_attraction;
    const double x = std::clamp(u, 0.0, 1.0) * attraction::GridSize;
    const double y = std::clamp(v, 0.0, 1.0) * attraction::GridSize;
    const auto column = std::min(static_cast<std::size_t>(x), attraction::GridSize - 1);
    const auto row = std::min(static_cast<std::size_t>(y), attraction::GridSize - 1);
    const double dx = x - column, dy = y - row;
    const auto tl = row * attraction::VertexSide + column;
    if (dx + dy <= 1.0)
        return frame.vertices[tl] * (1.0 - dx - dy) +
            frame.vertices[tl + 1] * dx + frame.vertices[tl + attraction::VertexSide] * dy;
    return frame.vertices[tl + attraction::VertexSide + 1] * (dx + dy - 1.0) +
        frame.vertices[tl + attraction::VertexSide] * (1.0 - dx) +
        frame.vertices[tl + 1] * (1.0 - dy);
}
}
