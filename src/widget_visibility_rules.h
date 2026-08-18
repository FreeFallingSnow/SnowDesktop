#pragma once

namespace snowdesktop::widget_visibility_rules
{
constexpr bool ShouldRenderWidget(
    bool showOnHoverOnly,
    bool itemDragActive,
    bool externalDragActive,
    bool widgetMoveActive,
    bool widgetSelected,
    bool interactionRetained,
    bool pointerInside)
{
    return !showOnHoverOnly ||
        itemDragActive ||
        externalDragActive ||
        widgetMoveActive ||
        widgetSelected ||
        interactionRetained ||
        pointerInside;
}

constexpr bool IsDesktopSurfaceVisible(
    bool desktopHidden,
    bool keepWhenDesktopHidden,
    bool hasDesktopBounds,
    bool interactionVisible)
{
    return (!desktopHidden || keepWhenDesktopHidden) &&
        hasDesktopBounds && interactionVisible;
}

constexpr bool ShouldPreserveHiddenPageRuntimeState(
    bool desktopHidden,
    bool hasDesktopBounds,
    bool pageUnavailable)
{
    return !desktopHidden && !hasDesktopBounds && pageUnavailable;
}

}
