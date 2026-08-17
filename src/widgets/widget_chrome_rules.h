#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::widget_chrome_rules
{

constexpr bool ReservesContentForBottomBar(
    bool showTitle, bool bottomBarHover) noexcept
{
    return showTitle && !bottomBarHover;
}

inline int ReservedBottomBarHeight(
    bool showTitle, bool bottomBarHover, int scaledBarHeight) noexcept
{
    return ReservesContentForBottomBar(showTitle, bottomBarHover)
        ? std::max(0, scaledBarHeight)
        : 0;
}

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

inline int BottomBarTitleTrailingReserve(
    int buttonCount, int buttonSize, int edgeGap,
    int betweenGap, int resizeReserve, int titleGap)
{
    const int count = std::max(0, buttonCount);
    return std::max(0, resizeReserve) +
        std::max(0, edgeGap) +
        count * std::max(0, buttonSize) +
        std::max(0, count - 1) * std::max(0, betweenGap) +
        std::max(0, titleGap);
}

} // namespace snowdesktop::widget_chrome_rules
