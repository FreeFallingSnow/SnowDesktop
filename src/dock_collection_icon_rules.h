#pragma once

#include <windows.h>

#include <algorithm>

namespace snowdesktop::dock_collection_icon_rules
{

inline constexpr int kContentPercent = 70;

struct Layout
{
    RECT background{};
    RECT content{};
    int gap = 0;
    int cellSize = 0;
    int groupSize = 0;
};

constexpr Layout CalculateLayout(
    const RECT& iconRect) noexcept
{
    Layout result;
    result.background = iconRect;
    const int width = std::max(
        0, static_cast<int>(
            iconRect.right - iconRect.left));
    const int height = std::max(
        0, static_cast<int>(
            iconRect.bottom - iconRect.top));
    const int outerSide =
        std::min(width, height);
    if (outerSide <= 0)
        return result;

    const int contentSide = std::max(
        1,
        (outerSide * kContentPercent + 50) /
            100);
    const int contentLeft =
        static_cast<int>(iconRect.left) +
        (width - contentSide) / 2;
    const int contentTop =
        static_cast<int>(iconRect.top) +
        (height - contentSide) / 2;
    result.content = {
        contentLeft,
        contentTop,
        contentLeft + contentSide,
        contentTop + contentSide
    };

    const int preferredGap = std::clamp(
        (contentSide * 4 + 50) / 100,
        2, 4);
    result.gap = std::min(
        preferredGap,
        std::max(0, contentSide - 2));
    result.cellSize = std::max(
        1, (contentSide - result.gap) / 2);
    result.groupSize =
        result.cellSize * 2 + result.gap;
    return result;
}

constexpr RECT CellRect(
    const Layout& layout,
    int column,
    int row) noexcept
{
    const int contentWidth = static_cast<int>(
        layout.content.right -
        layout.content.left);
    const int contentHeight = static_cast<int>(
        layout.content.bottom -
        layout.content.top);
    const int groupLeft =
        static_cast<int>(layout.content.left) +
        (contentWidth - layout.groupSize) / 2;
    const int groupTop =
        static_cast<int>(layout.content.top) +
        (contentHeight - layout.groupSize) / 2;
    const int left = groupLeft +
        std::clamp(column, 0, 1) *
            (layout.cellSize + layout.gap);
    const int top = groupTop +
        std::clamp(row, 0, 1) *
            (layout.cellSize + layout.gap);
    return {
        left, top,
        left + layout.cellSize,
        top + layout.cellSize
    };
}

} // namespace snowdesktop::dock_collection_icon_rules
