#pragma once

#include "dock_layout_settings.h"

#include <windows.h>

namespace snowdesktop::dock_rename_layout
{

RECT CalculateAdjacentEditRect(
    const RECT& anchorScreen, const RECT& workArea,
    DockPosition position, int desiredWidth,
    int desiredHeight, int gap, int margin);

} // namespace snowdesktop::dock_rename_layout
