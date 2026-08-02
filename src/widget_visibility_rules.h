#pragma once

namespace snowdesktop::widget_visibility_rules
{
constexpr bool ShouldRenderWidget(
    bool showOnHoverOnly,
    bool itemDragActive,
    bool externalDragActive,
    bool widgetMoveActive,
    bool widgetSelected,
    bool popupOpen,
    bool pointerInside)
{
    return !showOnHoverOnly ||
        itemDragActive ||
        externalDragActive ||
        widgetMoveActive ||
        widgetSelected ||
        popupOpen ||
        pointerInside;
}

constexpr bool ShouldRetainBackdropAfterDrag(
    bool showOnHoverOnly,
    bool widgetSelected,
    bool popupOpen,
    bool pointerInside)
{
    return ShouldRenderWidget(
        showOnHoverOnly,
        false,
        false,
        false,
        widgetSelected,
        popupOpen,
        pointerInside);
}

constexpr bool BecomesHiddenAfterPointerMove(
    bool showOnHoverOnly,
    bool widgetSelected,
    bool popupOpen,
    bool pointerWasInside,
    bool pointerIsInside)
{
    return ShouldRetainBackdropAfterDrag(
               showOnHoverOnly,
               widgetSelected,
               popupOpen,
               pointerWasInside) &&
        !ShouldRetainBackdropAfterDrag(
            showOnHoverOnly,
            widgetSelected,
            popupOpen,
            pointerIsInside);
}
}
