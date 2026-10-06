#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class LogicalSlotFocusDirection
{
    Left,
    Right,
    Up,
    Down,
};

struct LogicalSlotFocusRect
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

inline std::optional<std::size_t> CycleLogicalSlotFocus(
    std::size_t count, std::optional<std::size_t> current, bool reverse)
{
    if (count == 0) return std::nullopt;
    if (!current || *current >= count)
        return reverse ? count - 1 : 0;
    if (reverse)
        return *current == 0 ? count - 1 : *current - 1;
    return (*current + 1) % count;
}

inline std::optional<std::size_t> EnterWidgetKeyboardFocus(
    std::size_t count, std::optional<std::size_t> current,
    bool shift, bool alt, bool repeated)
{
    if (count == 0 || current || shift || alt || repeated)
        return std::nullopt;
    return 0;
}

inline std::optional<std::size_t> BeginAuxiliarySurfaceKeyboardFocus(
    std::size_t count, bool pending, bool alreadyFocused)
{
    if (count == 0 || !pending || alreadyFocused)
        return std::nullopt;
    return 0;
}

inline std::optional<std::size_t> MoveLogicalSlotItemTarget(
    std::size_t count, std::size_t current, int direction)
{
    if (current >= count || direction == 0) return std::nullopt;
    if (direction < 0)
        return current == 0 ? std::nullopt
                            : std::optional<std::size_t>(current - 1);
    return current + 1 < count
        ? std::optional<std::size_t>(current + 1) : std::nullopt;
}

inline std::optional<std::size_t> FindLogicalSlotSpatialFocus(
    const std::vector<LogicalSlotFocusRect>& items, std::size_t current,
    LogicalSlotFocusDirection direction)
{
    if (current >= items.size()) return std::nullopt;
    const auto& source = items[current];
    const long long sourceX = static_cast<long long>(source.left) + source.right;
    const long long sourceY = static_cast<long long>(source.top) + source.bottom;
    using Score = std::tuple<int, long long, long long, std::size_t>;
    std::optional<Score> best;
    std::optional<std::size_t> bestIndex;
    for (std::size_t index = 0; index < items.size(); ++index)
    {
        if (index == current) continue;
        const auto& candidate = items[index];
        const long long candidateX =
            static_cast<long long>(candidate.left) + candidate.right;
        const long long candidateY =
            static_cast<long long>(candidate.top) + candidate.bottom;
        const bool horizontal = direction == LogicalSlotFocusDirection::Left ||
            direction == LogicalSlotFocusDirection::Right;
        const bool forward = direction == LogicalSlotFocusDirection::Right ||
            direction == LogicalSlotFocusDirection::Down;
        const long long primaryDelta = horizontal
            ? candidateX - sourceX : candidateY - sourceY;
        if ((forward && primaryDelta <= 0) ||
            (!forward && primaryDelta >= 0))
            continue;
        const bool aligned = horizontal
            ? std::max(source.top, candidate.top) <
                std::min(source.bottom, candidate.bottom)
            : std::max(source.left, candidate.left) <
                std::min(source.right, candidate.right);
        const long long secondaryDelta = horizontal
            ? std::llabs(candidateY - sourceY)
            : std::llabs(candidateX - sourceX);
        const Score score{ aligned ? 0 : 1, std::llabs(primaryDelta),
            secondaryDelta, index };
        if (!best || score < *best)
        {
            best = score;
            bestIndex = index;
        }
    }
    return bestIndex;
}
}
