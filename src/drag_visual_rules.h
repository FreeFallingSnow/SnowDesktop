#pragma once

#include <windows.h>

#include <array>
#include <cstddef>

namespace snowdesktop::drag_visual_rules
{

struct ClipFragments
{
    std::array<RECT, 4> rects{};
    std::size_t count = 0;
};

constexpr bool HasArea(const RECT& rect) noexcept
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

/**
 * Split a dragged visual into the parts outside an owning foreground surface.
 *
 * The floating Dock renders the intersecting part in its own top-level
 * surface. Removing that same area from the desktop copy prevents the Dock's
 * backdrop brush from sampling a duplicate drag image underneath itself.
 */
constexpr ClipFragments ExcludeRect(
    const RECT& source, const RECT& excluded) noexcept
{
    ClipFragments result{};
    if (!HasArea(source))
        return result;

    const RECT intersection{
        source.left > excluded.left ? source.left : excluded.left,
        source.top > excluded.top ? source.top : excluded.top,
        source.right < excluded.right ? source.right : excluded.right,
        source.bottom < excluded.bottom ? source.bottom : excluded.bottom,
    };
    if (!HasArea(intersection))
    {
        result.rects[0] = source;
        result.count = 1;
        return result;
    }

    const auto append = [&](RECT rect) constexpr {
        if (HasArea(rect))
            result.rects[result.count++] = rect;
    };
    append(RECT{
        source.left, source.top,
        source.right, intersection.top });
    append(RECT{
        source.left, intersection.bottom,
        source.right, source.bottom });
    append(RECT{
        source.left, intersection.top,
        intersection.left, intersection.bottom });
    append(RECT{
        intersection.right, intersection.top,
        source.right, intersection.bottom });
    return result;
}

constexpr bool DropPreviewBelongsToRenderSurface(
    bool renderingFloatingDock,
    bool floatingDockOwnsDesktopCopy,
    bool targetIsFloatingDock) noexcept
{
    const bool belongsToFloatingDock =
        floatingDockOwnsDesktopCopy && targetIsFloatingDock;
    return renderingFloatingDock == belongsToFloatingDock;
}

} // namespace snowdesktop::drag_visual_rules
