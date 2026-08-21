#pragma once

namespace snowdesktop::widget_visibility_rules
{
constexpr bool ShouldRenderWidget(
    bool showOnHoverOnly,
    bool itemDragActive,
    bool externalDragActive,
    bool widgetMoveActive,
    bool widgetSelected,
    bool widgetFileSelected,
    bool interactionRetained,
    bool pointerInside)
{
    return !showOnHoverOnly ||
        itemDragActive ||
        externalDragActive ||
        widgetMoveActive ||
        widgetSelected ||
        widgetFileSelected ||
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

constexpr bool ShouldKeepTopologyHiddenPageRuntimeActive(
    bool desktopHidden,
    bool desktopSurfaceVisible,
    bool pageUnavailable,
    bool hiddenByDisplayTopology)
{
    return !desktopHidden && !desktopSurfaceVisible &&
        pageUnavailable && hiddenByDisplayTopology;
}

constexpr bool ShouldHideWidgetPreviewSource(
    bool widgetMoveActive,
    bool widgetResizeActive,
    bool isPreviewSource)
{
    return isPreviewSource &&
        (widgetMoveActive || widgetResizeActive);
}

}
