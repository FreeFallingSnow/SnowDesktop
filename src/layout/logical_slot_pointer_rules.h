#pragma once

#include "widget/runtime/widget_item_layout.h"
#include "widget/runtime/widget_logical_slot.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

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
ResolveLogicalSlotInsertionTarget(std::span<const RECT> items,
    POINT point, const RECT& surface) noexcept
{
    if (surface.right <= surface.left || surface.bottom <= surface.top ||
        !PtInRect(&surface, point))
        return std::nullopt;

    const auto centerX = [](const RECT& value) {
        return value.left + (value.right - value.left) / 2;
    };
    const auto centerY = [](const RECT& value) {
        return value.top + (value.bottom - value.top) / 2;
    };
    const bool horizontal = items.size() >= 2 &&
        std::abs(centerX(items[1]) - centerX(items[0])) >
            std::abs(centerY(items[1]) - centerY(items[0]));

    if (items.empty())
    {
        constexpr LONG halfThickness = 2;
        const LONG y = std::min(surface.bottom - halfThickness,
            surface.top + std::max<LONG>(4, halfThickness));
        const RECT indicator{
            surface.left + std::min<LONG>(4,
                std::max<LONG>(0, (surface.right - surface.left) / 4)),
            y - halfThickness,
            surface.right - std::min<LONG>(4,
                std::max<LONG>(0, (surface.right - surface.left) / 4)),
            y + halfThickness };
        if (indicator.right <= indicator.left ||
            indicator.bottom <= indicator.top)
            return std::nullopt;
        return LogicalSlotPointerTarget{ 0, 0, indicator, false };
    }

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

    const RECT& anchor = insertionIndex < items.size()
        ? items[insertionIndex] : items.back();
    constexpr LONG halfThickness = 2;
    RECT indicator{};
    if (horizontal)
    {
        LONG x = insertionIndex < items.size()
            ? anchor.left : anchor.right;
        if (insertionIndex > 0 && insertionIndex < items.size() &&
            widget_item_layout::SharesInsertionBoundary(
                items[insertionIndex - 1], anchor, true))
            x -= (anchor.left - items[insertionIndex - 1].right) / 2;
        indicator = { x - halfThickness,
            std::max(surface.top, anchor.top), x + halfThickness,
            std::min(surface.bottom, anchor.bottom) };
    }
    else
    {
        LONG y = insertionIndex < items.size()
            ? anchor.top : anchor.bottom;
        if (insertionIndex > 0 && insertionIndex < items.size() &&
            widget_item_layout::SharesInsertionBoundary(
                items[insertionIndex - 1], anchor, false))
            y -= (anchor.top - items[insertionIndex - 1].bottom) / 2;
        indicator = { std::max(surface.left, anchor.left),
            y - halfThickness, std::min(surface.right, anchor.right),
            y + halfThickness };
    }
    if (indicator.right <= indicator.left ||
        indicator.bottom <= indicator.top)
        return std::nullopt;
    return LogicalSlotPointerTarget{
        insertionIndex, insertionIndex, indicator, horizontal };
}

inline std::optional<LogicalSlotPointerTarget>
ResolveLogicalSlotPointerTarget(std::span<const RECT> items,
    std::size_t sourceIndex, POINT point, const RECT& surface) noexcept
{
    if (items.size() < 2 || sourceIndex >= items.size())
        return std::nullopt;
    auto target = ResolveLogicalSlotInsertionTarget(items, point, surface);
    if (!target) return std::nullopt;
    target->targetIndex = std::min(items.size() - 1,
        target->insertionIndex > sourceIndex
            ? target->insertionIndex - 1 : target->insertionIndex);
    return target;
}

// The rectangles describe a clipped projection. Translate its anchor identity
// into the complete model before applying Move/Transfer or persisting an order.
inline std::optional<LogicalSlotPointerTarget>
ResolveLogicalSlotModelPointerTarget(std::span<const RECT> items,
    std::span<const std::string_view> visibleIds,
    const LogicalSlotSnapshot& model, std::string_view sourceId,
    POINT point, const RECT& surface)
{
    if (items.size() != visibleIds.size()) return std::nullopt;
    std::size_t previous = 0;
    bool first = true;
    for (const auto id : visibleIds)
    {
        const auto found = std::find_if(model.items.begin(), model.items.end(),
            [id](const auto& item) { return item.id == id; });
        if (found == model.items.end()) return std::nullopt;
        const auto index = static_cast<std::size_t>(found - model.items.begin());
        if (!first && index <= previous) return std::nullopt;
        previous = index;
        first = false;
    }
    auto target = ResolveLogicalSlotInsertionTarget(items, point, surface);
    if (!target) return std::nullopt;
    std::size_t insertion = model.items.size();
    if (!visibleIds.empty())
    {
        const bool afterLast = target->insertionIndex == visibleIds.size();
        const auto anchor = afterLast ? visibleIds.back()
            : visibleIds[target->insertionIndex];
        const auto found = std::find_if(model.items.begin(), model.items.end(),
            [anchor](const auto& item) { return item.id == anchor; });
        insertion = static_cast<std::size_t>(found - model.items.begin()) +
            (afterLast ? 1 : 0);
    }
    target->insertionIndex = target->targetIndex = insertion;
    if (!sourceId.empty())
    {
        const auto source = std::find_if(model.items.begin(), model.items.end(),
            [sourceId](const auto& item) { return item.id == sourceId; });
        if (source == model.items.end()) return std::nullopt;
        const auto sourceIndex = static_cast<std::size_t>(source - model.items.begin());
        target->targetIndex = insertion > sourceIndex ? insertion - 1 : insertion;
    }
    return target;
}
}
