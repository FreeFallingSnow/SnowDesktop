#pragma once

#include "widget_accessibility_snapshot.h"

#include <functional>
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
    AriaRole,
    HelpText,
    ItemStatus,
    HeadingLevel,
    LiveSetting,
    PositionInSet,
    SizeOfSet,
    LiveRegion,
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

// Event-only state. Provider queries always capture the live runtime separately.
class WidgetAccessibilityEventState final
{
public:
    using Snapshots = std::vector<LuaWidgetAccessibilitySnapshot>;
    using Capture = std::function<bool(Snapshots&)>;
    using Listening = std::function<bool()>;
    struct Update
    {
        Snapshots previous;
        std::vector<WidgetAccessibilityChange> changes;
    };

    void Reset() noexcept;
    Update Refresh(const Capture& capture, const Listening& clientsAreListening);
    const Snapshots& Current() const noexcept { return current_; }

private:
    Snapshots current_;
};
}
