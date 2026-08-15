#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace snowdesktop::widget_runtime
{
struct LogicalSlotPointerTarget
{
    std::size_t insertionIndex = 0;
    std::size_t targetIndex = 0;
    RECT indicator{};
    bool horizontal = false;
};

inline std::optional<LogicalSlotPointerTarget>
ResolveLogicalSlotPointerTarget(std::span<const RECT> items,
    std::size_t sourceIndex, POINT point, const RECT& surface) noexcept
{
    if (items.size() < 2 || sourceIndex >= items.size() ||
        surface.right <= surface.left || surface.bottom <= surface.top)
        return std::nullopt;

    const auto centerX = [](const RECT& value) {
        return value.left + (value.right - value.left) / 2;
    };
    const auto centerY = [](const RECT& value) {
        return value.top + (value.bottom - value.top) / 2;
    };
    const LONG deltaX = std::abs(centerX(items[1]) - centerX(items[0]));
    const LONG deltaY = std::abs(centerY(items[1]) - centerY(items[0]));
    const bool horizontal = deltaX > deltaY;

    std::size_t insertionIndex = items.size();
    bool hit = false;
    for (std::size_t index = 0; index < items.size(); ++index)
    {
        if (!PtInRect(&items[index], point)) continue;
        insertionIndex = (horizontal ? point.x < centerX(items[index])
                                     : point.y < centerY(items[index]))
            ? index : index + 1;
        hit = true;
        break;
    }
    if (!hit)
    {
        const LONG coordinate = horizontal ? point.x : point.y;
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            const LONG center = horizontal
                ? centerX(items[index]) : centerY(items[index]);
            if (coordinate < center)
            {
                insertionIndex = index;
                break;
            }
        }
    }

    const std::size_t targetIndex = std::min(items.size() - 1,
        insertionIndex > sourceIndex
            ? insertionIndex - 1 : insertionIndex);
    const RECT& anchor = insertionIndex < items.size()
        ? items[insertionIndex] : items.back();
    constexpr LONG halfThickness = 2;
    RECT indicator{};
    if (horizontal)
    {
        const LONG x = insertionIndex < items.size()
            ? anchor.left : anchor.right;
        indicator = { x - halfThickness,
            std::max(surface.top, anchor.top), x + halfThickness,
            std::min(surface.bottom, anchor.bottom) };
    }
    else
    {
        const LONG y = insertionIndex < items.size()
            ? anchor.top : anchor.bottom;
        indicator = { std::max(surface.left, anchor.left),
            y - halfThickness, std::min(surface.right, anchor.right),
            y + halfThickness };
    }
    if (indicator.right <= indicator.left ||
        indicator.bottom <= indicator.top)
        return std::nullopt;
    return LogicalSlotPointerTarget{
        insertionIndex, targetIndex, indicator, horizontal };
}
}
