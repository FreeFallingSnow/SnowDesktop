#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>

namespace snowdesktop::page_navigation_rules
{

constexpr int kHotEdgeWidthDip = 8;
constexpr UINT kHotEdgeHintDelayMs = 500;

enum class PointerTarget
{
    None,
    PreviousEdge,
    NextEdge,
};

inline int HotEdgeWidth(UINT dpi) noexcept
{
    return std::max(1, MulDiv(
        kHotEdgeWidthDip,
        dpi == 0 ? USER_DEFAULT_SCREEN_DPI : static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

inline void BuildHotEdgeRects(
    const RECT& workArea,
    UINT dpi,
    RECT& previous,
    RECT& next) noexcept
{
    previous = {};
    next = {};
    if (workArea.right <= workArea.left ||
        workArea.bottom <= workArea.top)
        return;

    const LONG width = std::min<LONG>(
        HotEdgeWidth(dpi),
        workArea.right - workArea.left);
    previous = {
        workArea.left,
        workArea.top,
        workArea.left + width,
        workArea.bottom,
    };
    next = {
        workArea.right - width,
        workArea.top,
        workArea.right,
        workArea.bottom,
    };
}

inline PointerTarget HitTestPointerTarget(
    POINT point,
    const RECT& previousEdge,
    const RECT& nextEdge) noexcept
{
    if (PtInRect(&previousEdge, point))
        return PointerTarget::PreviousEdge;
    if (PtInRect(&nextEdge, point))
        return PointerTarget::NextEdge;
    return PointerTarget::None;
}

inline int PointerTargetDirection(PointerTarget target) noexcept
{
    switch (target)
    {
    case PointerTarget::PreviousEdge:
        return -1;
    case PointerTarget::NextEdge:
        return 1;
    case PointerTarget::None:
    default:
        return 0;
    }
}

inline bool IsEdgeTarget(PointerTarget target) noexcept
{
    return target == PointerTarget::PreviousEdge ||
        target == PointerTarget::NextEdge;
}

inline UINT NormalizeModifiers(UINT modifiers) noexcept
{
    return modifiers &
        (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
}

inline bool IsReservedDesktopSingleKey(
    UINT modifiers, UINT virtualKey) noexcept
{
    if (NormalizeModifiers(modifiers) != 0)
        return false;
    switch (virtualKey)
    {
    case VK_F2:
    case VK_F5:
    case VK_DELETE:
    case VK_RETURN:
    case VK_ESCAPE:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
        return true;
    default:
        return false;
    }
}

inline bool ShortcutMatches(
    UINT configuredModifiers,
    UINT configuredVirtualKey,
    UINT pressedModifiers,
    UINT pressedVirtualKey) noexcept
{
    return configuredVirtualKey != 0 &&
        configuredVirtualKey == pressedVirtualKey &&
        NormalizeModifiers(configuredModifiers) ==
            NormalizeModifiers(pressedModifiers);
}

template <typename HasContentAtPageIndex>
int NextNonEmptyOffset(
    int fromOffset,
    int direction,
    std::size_t savedPageCount,
    std::size_t gridPageCount,
    HasContentAtPageIndex&& hasContentAtPageIndex)
{
    if (savedPageCount == 0 || gridPageCount == 0 ||
        (direction != -1 && direction != 1))
        return fromOffset;

    const int visiblePageCount = static_cast<int>(
        std::min(savedPageCount, gridPageCount));
    const int rawMaximum = std::max(
        0, static_cast<int>(savedPageCount) - visiblePageCount);
    int offset = fromOffset;
    while (true)
    {
        offset += direction;
        if (offset < 0 || offset > rawMaximum)
            return fromOffset;
        const std::size_t pageIndex = static_cast<std::size_t>(
            visiblePageCount - 1 + offset);
        if (pageIndex < savedPageCount &&
            hasContentAtPageIndex(pageIndex))
            return offset;
    }
}

template <typename HasContentAtPageIndex>
int MaximumOffset(
    std::size_t savedPageCount,
    std::size_t gridPageCount,
    HasContentAtPageIndex&& hasContentAtPageIndex)
{
    if (savedPageCount == 0 || gridPageCount == 0)
        return 0;

    const int visiblePageCount = static_cast<int>(
        std::min(savedPageCount, gridPageCount));
    const int rawMaximum = std::max(
        0, static_cast<int>(savedPageCount) - visiblePageCount);
    for (int offset = rawMaximum; offset > 0; --offset)
    {
        const std::size_t pageIndex = static_cast<std::size_t>(
            visiblePageCount - 1 + offset);
        if (pageIndex < savedPageCount &&
            hasContentAtPageIndex(pageIndex))
            return offset;
    }
    return 0;
}

} // namespace snowdesktop::page_navigation_rules
