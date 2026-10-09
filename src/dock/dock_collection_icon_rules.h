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
    const int preferredGap = std::clamp(
        (contentSide * 4 + 50) / 100,
        2, 4);
    result.gap = std::min(
        preferredGap,
        std::max(0, contentSide - 2));
    result.cellSize = std::max(
        1, (contentSide - result.gap) / 2);
    // Retain thumbnail sizes as the Dock grows. Adjust only the gap's
    // parity so two equal cells can have equal integer outer margins.
    if (outerSide > 1 && (outerSide - result.gap) % 2 != 0)
        result.gap += result.gap >= 3 ? -1 : 1;
    result.groupSize =
        result.cellSize * 2 + result.gap;
    // Center the actual group once, rather than truncating both an inner
    // frame's inset and the group's offset within that frame.
    const int contentLeft =
        static_cast<int>(iconRect.left) +
        (width - result.groupSize) / 2;
    const int contentTop =
        static_cast<int>(iconRect.top) +
        (height - result.groupSize) / 2;
    result.content = {
        contentLeft,
        contentTop,
        contentLeft + result.groupSize,
        contentTop + result.groupSize
    };
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
