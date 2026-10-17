#pragma once

#include "common/types.h"
#include "common/constants.h"
#include <algorithm>
#include <cmath>

namespace snowdesktop::single_cell_widget
{
constexpr bool Supports(DesktopWidgetType type) noexcept
{
    return type == DesktopWidgetType::Collection ||
        type == DesktopWidgetType::FileCategories ||
        type == DesktopWidgetType::FolderMapping ||
        IsGroupWidgetType(type);
}

inline bool IsSingleCell(const DesktopWidget& widget) noexcept
{
    return Supports(widget.type) &&
        widget.gridSpan.columns == 1 && widget.gridSpan.rows == 1;
}

inline bool IsDesktopSingleCell(const DesktopWidget& widget) noexcept
{
    return widget.gridCell.pageId != kDockPageId && IsSingleCell(widget);
}

inline bool IsIconMode(const DesktopWidget& widget) noexcept
{
    return widget.singleCellIconMode && IsDesktopSingleCell(widget);
}

struct ArcHandle
{
    float centerX = 0;
    float centerY = 0;
    float radius = 0;
    float stroke = 0;
    float hitWidth = 0;
};

inline ArcHandle ResolveArc(RECT icon, float scale) noexcept
{
    const float edge = static_cast<float>(std::max<LONG>(0,
        std::min(icon.right - icon.left, icon.bottom - icon.top)));
    scale = std::max(0.1f, scale);
    // The quarter circle sits outside the icon; its radius leaves a visible
    // gap even at the rounded ends of the stroke.
    const float radius = std::min(edge * 0.25f, 5.0f * scale);
    return {static_cast<float>(icon.right), static_cast<float>(icon.bottom),
        radius, std::min(edge * 0.09f, 2.5f * scale),
        std::min(edge * 0.18f, 3.0f * scale)};
}

inline RECT ArcBounds(const ArcHandle& arc, bool hitTarget = false) noexcept
{
    if (arc.radius <= 0) return {};
    const float margin = hitTarget ? arc.hitWidth : arc.stroke * 0.5f + 1.0f;
    return {static_cast<LONG>(std::floor(arc.centerX - margin)),
        static_cast<LONG>(std::floor(arc.centerY - margin)),
        static_cast<LONG>(std::ceil(arc.centerX + arc.radius + margin)),
        static_cast<LONG>(std::ceil(arc.centerY + arc.radius + margin))};
}

inline bool HitArc(const ArcHandle& arc, POINT point) noexcept
{
    if (arc.radius <= 0) return false;
    const float dx = static_cast<float>(point.x) - arc.centerX;
    const float dy = static_cast<float>(point.y) - arc.centerY;
    return (dx > 0 || dy > 0) && dx >= -arc.hitWidth && dy >= -arc.hitWidth &&
        std::fabs(std::hypot(dx, dy) - arc.radius) <= arc.hitWidth;
}

// Same four-tile geometry as the existing compact collection.
inline RECT ThumbnailCell(RECT body, size_t index, int padding) noexcept
{
    if (index >= 4 || IsRectEmpty(&body)) return {};
    InflateRect(&body, -padding, -padding);
    const int width = std::max<LONG>(1, body.right - body.left);
    const int height = std::max<LONG>(1, body.bottom - body.top);
    const int edge = std::max(1, std::min(width, height) / 2);
    const int extraX = std::max(0, width - edge * 2);
    const int extraY = std::max(0, height - edge * 2);
    const int gapX = extraX >= 3 ? extraX / 3 : (extraX > 0 ? 1 : 0);
    const int gapY = extraY >= 3 ? extraY / 3 : (extraY > 0 ? 1 : 0);
    const int left = body.left + (width - edge * 2 - gapX) / 2 +
        static_cast<int>(index % 2) * (edge + gapX);
    const int top = body.top + (height - edge * 2 - gapY) / 2 +
        static_cast<int>(index / 2) * (edge + gapY);
    return {left, top, left + edge, top + edge};
}

inline RECT ThumbnailIconRect(RECT tile, int horizontalInset,
    int verticalInset, int minimumIcon) noexcept
{
    const int width = tile.right - tile.left;
    const int height = tile.bottom - tile.top;
    const int available = std::max(1, std::min(width - horizontalInset, height - verticalInset));
    const int edge = std::min(std::max(minimumIcon, available), std::max(1, std::min(width, height)));
    const int left = tile.left + (width - edge) / 2;
    const int top = tile.top + (height - edge) / 2;
    return {left, top, left + edge, top + edge};
}
}
