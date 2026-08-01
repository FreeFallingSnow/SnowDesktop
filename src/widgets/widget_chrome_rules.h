#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::widget_chrome_rules
{

/**
 * Return the horizontal inset that keeps the bottom-bar edge content inside
 * the rounded widget outline.
 *
 * Bottom-bar glyphs occupy at most roughly two thirds of the configured bar
 * height.  Sampling the rounded corner at the glyph's lowest edge gives the
 * extra inset required by large corner radii while retaining the established
 * minimum breathing room for ordinary settings.
 */
inline int BottomBarSideInset(
    int cornerRadius, int barHeight,
    int minimumInset, int bottomGap)
{
    const int radius = std::max(0, cornerRadius);
    const int height = std::max(1, barHeight);
    const int baseInset = std::max(0, minimumInset);
    const int gap = std::max(0, bottomGap);

    // The lowest point of a glyph whose height is 2/3 of the bar height is
    // bottomGap + barHeight/6 above the bottom edge.
    const double clearance = static_cast<double>(gap) +
        static_cast<double>(height) / 6.0;
    const double cornerOffset = std::max(
        0.0, static_cast<double>(radius) - clearance);
    if (cornerOffset <= 0.0 || radius == 0)
        return baseInset;

    const double squaredInside = std::max(
        0.0, static_cast<double>(radius) * radius -
            cornerOffset * cornerOffset);
    const int roundedInset = static_cast<int>(std::ceil(
        static_cast<double>(radius) - std::sqrt(squaredInside)));
    return std::max(baseInset, roundedInset);
}

} // namespace snowdesktop::widget_chrome_rules
