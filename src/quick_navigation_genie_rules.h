#pragma once

#include "dock_genie_rules.h"

namespace snowdesktop::quick_navigation_animation_rules
{
inline dock_genie::Rect GenieContentClip(std::size_t index,
    double width, double height, dock_genie::Edge edge)
{
    if (index >= dock_genie::StripCount)
        return {};
    const bool vertical = dock_genie::Vertical(edge);
    const double axis = vertical ? height : width;
    const double begin = axis * static_cast<double>(index) / dock_genie::StripCount;
    const double end = axis * static_cast<double>(index + 1) / dock_genie::StripCount;
    // Alpha content must cover each source pixel once; overlapping adjacent
    // clips composites its translucent background twice and creates dark seams.
    return vertical ? dock_genie::Rect{0.0, begin, width, end}
        : dock_genie::Rect{begin, 0.0, end, height};
}
}
