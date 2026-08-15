#pragma once

#include "widget_view_accessibility.h"

#include <string>
#include <vector>

#include <windows.h>

struct LuaWidgetAccessibilitySnapshot
{
    std::wstring widgetId;
    std::string packageId;
    std::string name;
    RECT bounds{};
    bool selected = false;
    std::vector<snowdesktop::widget_runtime::ViewAccessibilityNode> nodes;
    std::string error;
};
