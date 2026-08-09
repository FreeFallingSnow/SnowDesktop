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

}
