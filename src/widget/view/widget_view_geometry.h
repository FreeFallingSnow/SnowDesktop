#pragma once

#include "widget_view_tree.h"

#include <algorithm>
#include <optional>

namespace snowdesktop::widget_runtime::detail
{
inline std::optional<ViewRect> IntersectRects(
    const std::optional<ViewRect>& first, const ViewRect& second) noexcept
{
    if (!first) return second;
    const float left = std::max(first->x, second.x);
    const float top = std::max(first->y, second.y);
    const float right = std::min(first->x + first->width,
        second.x + second.width);
    const float bottom = std::min(first->y + first->height,
        second.y + second.height);
    if (right <= left || bottom <= top) return std::nullopt;
    return ViewRect{ left, top, right - left, bottom - top };
}

struct TransformedPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

inline TransformedPoint ApplyTransform(float x, float y,
    const ViewResolvedTransform& transform) noexcept
{
    return { x * transform.m11 + y * transform.m21 + transform.dx,
        x * transform.m12 + y * transform.m22 + transform.dy };
}
}
