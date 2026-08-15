#pragma once

#include "widget_accessibility_snapshot.h"

#include <string>
#include <vector>

namespace snowdesktop
{
enum class WidgetAccessibilityElementKind
{
    Root,
    Widget,
    Node,
};

enum class WidgetAccessibilityChangeKind
{
    Structure,
    Focus,
    Name,
    Enabled,
    Required,
    Offscreen,
    Toggle,
    SelectionItem,
    RangeValue,
    Value,
    ExpandCollapse,
    Bounds,
    HorizontalScrollPercent,
    HorizontalViewSize,
    HorizontallyScrollable,
    VerticalScrollPercent,
    VerticalViewSize,
    VerticallyScrollable,
};

struct WidgetAccessibilityChange
{
    WidgetAccessibilityChangeKind kind =
        WidgetAccessibilityChangeKind::Structure;
    WidgetAccessibilityElementKind element =
        WidgetAccessibilityElementKind::Root;
    std::wstring widgetId;
    std::string semanticId;

    bool operator==(const WidgetAccessibilityChange&) const = default;
};

std::vector<WidgetAccessibilityChange> DiffWidgetAccessibilitySnapshots(
    const std::vector<LuaWidgetAccessibilitySnapshot>& previous,
    const std::vector<LuaWidgetAccessibilitySnapshot>& current);
}
