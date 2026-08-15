#pragma once

#include "widget_view_accessibility.h"

#include <string>
#include <vector>

#include <windows.h>

enum class LuaWidgetAccessibilityActionKind
{
    Invoke,
    Toggle,
    Select,
    AddToSelection,
    RemoveFromSelection,
    SetRangeValue,
    SetValue,
    Expand,
    Collapse,
    SetScrollOffset,
};

struct LuaWidgetAccessibilityActionRequest
{
    LuaWidgetAccessibilityActionKind kind =
        LuaWidgetAccessibilityActionKind::Invoke;
    std::wstring widgetId;
    std::string nodeKey;
    double numericValue = 0.0;
    std::wstring textValue;
};

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
