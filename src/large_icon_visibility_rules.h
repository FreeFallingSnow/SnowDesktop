#pragma once
#include "large_icon_config.h"
#include "widget_visibility_rules.h"

namespace snowdesktop::large_icon_visibility_rules
{
inline bool Visible(const LargeIconConfig& config, bool desktopHidden,
    bool onDesktop, bool pointerInside, bool selected, bool dragging, bool interactionRetained)
{
    return widget_visibility_rules::IsDesktopSurfaceVisible(desktopHidden,
        config.keepWhenDesktopHidden, onDesktop,
        widget_visibility_rules::ShouldRenderWidget(config.showOnHoverOnly,
            dragging, false, false, selected, false, interactionRetained, pointerInside));
}
}
