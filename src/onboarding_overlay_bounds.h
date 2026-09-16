#pragma once

#include <windows.h>
#include <vector>

// Private UI-thread geometry for teaching-tip avoidance, not a component API.
namespace snowdesktop::modern_menu
{
std::vector<RECT> ActivePopupBounds();
}
namespace snowdesktop::component_preview
{
RECT ActivePreviewBounds();
}
