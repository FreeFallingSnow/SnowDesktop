#pragma once

namespace snowdesktop::desktop_hover_rules
{
constexpr bool CanClearPassiveHover(
    bool ownsInteractionCapture,
    bool mouseDown,
    bool dragActive,
    bool widgetInteractionActive)
{
    return !ownsInteractionCapture &&
        !mouseDown &&
        !dragActive &&
        !widgetInteractionActive;
}
}
