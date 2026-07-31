#pragma once

namespace snowdesktop::widget_visibility_rules
{
constexpr bool ShouldRenderWidget(
    bool showOnHoverOnly,
    bool itemDragActive,
    bool externalDragActive,
    bool widgetMoveActive,
    bool popupOpen,
    bool pointerInside)
{
    return !showOnHoverOnly ||
        itemDragActive ||
        externalDragActive ||
        widgetMoveActive ||
        popupOpen ||
        pointerInside;
}

constexpr bool ShouldRetainBackdropAfterDrag(
    bool showOnHoverOnly,
    bool popupOpen,
    bool pointerInside)
{
    return ShouldRenderWidget(
        showOnHoverOnly,
        false,
        false,
        false,
        popupOpen,
        pointerInside);
}
}
