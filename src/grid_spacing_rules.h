#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace snowdesktop::grid_spacing_rules
{

struct AxisGeometry
{
    int extent = 1;
    int count = 1;
    int baseMargin = 0;
    int margin = 0;
    int cell = 1;
    int gap = 0;
};

inline AxisGeometry ResolveAxis(
    int extent, int count, int baseMargin,
    float gapPercent, int minimumCell, float spacingScale)
{
    AxisGeometry result;
    result.extent = std::max(1, extent);
    result.count = std::max(1, count);
    result.baseMargin = std::max(0, baseMargin);

    const int innerExtent = std::max(
        result.count,
        result.extent - result.baseMargin * 2);
    if (result.count <= 1)
    {
        result.margin = result.baseMargin;
        result.cell = std::max(1, innerExtent);
        return result;
    }

    const float pitch = static_cast<float>(innerExtent) /
        static_cast<float>(result.count);
    const int maximumGap = std::max(
        0, (innerExtent - result.count) / result.count);
    const int minimumCellGapLimit = std::max(
        0, (innerExtent - result.count *
            std::max(1, minimumCell)) / result.count);
    result.gap = std::clamp(
        static_cast<int>(std::round(
            pitch * gapPercent *
            std::clamp(spacingScale, 0.5f, 2.0f))),
        0, std::min(maximumGap, minimumCellGapLimit));
    result.margin = result.baseMargin + result.gap / 2;

    const int usableExtent = std::max(
        result.count,
        result.extent - result.margin * 2);
    result.cell = std::max(
        1, (usableExtent -
            result.gap * (result.count - 1)) /
            result.count);
    return result;
}

inline int TrackCenter(const AxisGeometry& axis, int index)
{
    const int count = std::max(1, axis.count);
    const int clampedIndex = std::clamp(index, 0, count - 1);
    const int innerExtent = std::max(
        count, axis.extent - std::max(0, axis.baseMargin) * 2);
    const std::int64_t numerator =
        static_cast<std::int64_t>(clampedIndex * 2 + 1) *
        static_cast<std::int64_t>(innerExtent);
    const int relativeCenter = static_cast<int>(
        (numerator + count) /
        static_cast<std::int64_t>(count * 2));
    return std::max(0, axis.baseMargin) + relativeCenter;
}

inline int CellStart(const AxisGeometry& axis, int index)
{
    return TrackCenter(axis, index) - std::max(1, axis.cell) / 2;
}

inline int CellEnd(const AxisGeometry& axis, int index)
{
    return CellStart(axis, index) + std::max(1, axis.cell);
}

} // namespace snowdesktop::grid_spacing_rules
