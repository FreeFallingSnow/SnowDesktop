#pragma once

#include "dock_attraction_rules.h"
#include "dock_genie_timeline.h"

namespace snowdesktop::dock_attraction
{
inline constexpr std::size_t TimelineSteps = 8;
// Affine clip-parent matrix (six coefficients), then whole-mesh opacity.
using Values = std::array<double, 7>;
template <std::size_t Grid>
struct Mesh
{
    static constexpr std::size_t Side = Grid + 1;
    static constexpr std::size_t Triangles = Grid * Grid * 2;
    static constexpr std::size_t Vertices = Side * Side;
    std::array<Point, Vertices> vertices{};
    double opacity = 0;
};
using Frame = Mesh<GridSize>;
inline constexpr std::size_t VertexSide = Frame::Side;
inline constexpr std::size_t VertexCount = Frame::Vertices;
inline constexpr std::size_t TriangleCount = Frame::Triangles;

template <std::size_t Grid = GridSize>
inline std::array<std::size_t, 3> TriangleVertices(std::size_t triangle) noexcept
{
    constexpr auto side = Mesh<Grid>::Side;
    const auto cell = triangle / 2, row = cell / Grid, column = cell % Grid;
    const auto tl = row * side + column, tr = tl + 1, bl = tl + side, br = bl + 1;
    return triangle % 2 == 0 ? std::array<std::size_t, 3>{tl, tr, bl} : std::array<std::size_t, 3>{br, bl, tr};
}
template <std::size_t Grid = GridSize>
inline Mesh<Grid> MeshFrame(Rect window, Rect dock, double collapsed) noexcept
{
    Mesh<Grid> frame{};
    constexpr auto side = Mesh<Grid>::Side;
    const auto patch = ControlPoints(window, dock, collapsed);
    for (std::size_t i = 0; i < frame.vertices.size(); ++i)
        frame.vertices[i] = Surface(patch, static_cast<double>(i % side) / Grid,
                                    static_cast<double>(i / side) / Grid);
    frame.opacity = Opacity(collapsed);
    return frame;
}
template <std::size_t Grid = GridSize>
inline Mesh<Grid> TimelineFrame(Rect window, Rect dock, double from, double to, double progress) noexcept
{
    return MeshFrame<Grid>(window, dock, dock_genie::Mix(from, to, dock_genie::Smooth(progress)));
}
template <std::size_t Grid>
inline Values TriangleValues(const Mesh<Grid> &frame, std::size_t triangle, double hostX, double hostY) noexcept
{
    const auto indices = TriangleVertices<Grid>(triangle);
    const auto a = frame.vertices[indices[0]], b = frame.vertices[indices[1]], c = frame.vertices[indices[2]];
    const Point horizontal = b - a, diagonal = c - b;
    // The image maps its triangle corners to (0,0), (1,0), (1,1).
    // A single s<=1 clip selects the triangle, including mirrored cells.
    return {horizontal.x, horizontal.y, diagonal.x, diagonal.y, a.x - hostX, a.y - hostY, frame.opacity};
}
inline double MonotoneTangent(double previous, double current, double next) noexcept
{
    const double before = current - previous, after = next - current;
    return before * after > 0 ? 2 * before * after / (before + after) : 0;
}
template <std::size_t Grid>
inline std::array<Mesh<Grid>, TimelineSteps + 1>
MeshTangents(const std::array<Mesh<Grid>, TimelineSteps + 1> &frames) noexcept
{
    std::array<Mesh<Grid>, TimelineSteps + 1> tangents{};
    // Each vertex gets exactly one trajectory. All incident triangles use
    // these same positions and tangents, even at folds and on reversals.
    for (std::size_t step = 1; step < TimelineSteps; ++step)
    {
        for (std::size_t vertex = 0; vertex < frames[step].vertices.size(); ++vertex)
        {
            const auto previous = frames[step - 1].vertices[vertex], current = frames[step].vertices[vertex],
                       next = frames[step + 1].vertices[vertex];
            tangents[step].vertices[vertex] = {MonotoneTangent(previous.x, current.x, next.x),
                                               MonotoneTangent(previous.y, current.y, next.y)};
        }
        tangents[step].opacity =
            MonotoneTangent(frames[step - 1].opacity, frames[step].opacity, frames[step + 1].opacity);
    }
    return tangents;
}
inline dock_genie::TimelineCubic TimelineSegment(const Values &first, const Values &last, const Values &firstTangent,
                                                 const Values &lastTangent, std::size_t element,
                                                 double duration) noexcept
{
    const double dt = duration / TimelineSteps, delta = last[element] - first[element];
    const double a = firstTangent[element], b = lastTangent[element];
    return {static_cast<float>(first[element]), static_cast<float>(a / dt),
            static_cast<float>((3 * delta - 2 * a - b) / (dt * dt)),
            static_cast<float>((-2 * delta + a + b) / (dt * dt * dt))};
}
} // namespace snowdesktop::dock_attraction
