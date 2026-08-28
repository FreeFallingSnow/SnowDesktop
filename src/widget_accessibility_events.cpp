#include "widget_accessibility_events.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>

namespace snowdesktop
{
namespace
{
using AccessibilityNode =
    snowdesktop::widget_runtime::ViewAccessibilityNode;

struct NodeKey
{
    std::wstring widgetId;
    std::string semanticId;

    bool operator<(const NodeKey& other) const noexcept
    {
        return std::tie(widgetId, semanticId) <
            std::tie(other.widgetId, other.semanticId);
    }

    bool operator==(const NodeKey&) const = default;
};

struct NodeEntry
{
    const LuaWidgetAccessibilitySnapshot* widget = nullptr;
    const AccessibilityNode* node = nullptr;
};

std::map<NodeKey, NodeEntry> IndexNodes(
    const std::vector<LuaWidgetAccessibilitySnapshot>& snapshots)
{
    std::map<NodeKey, NodeEntry> result;
    for (const auto& widget : snapshots)
        for (const auto& node : widget.nodes)
            result.emplace(NodeKey{ widget.widgetId, node.semanticId },
                NodeEntry{ &widget, &node });
    return result;
}

std::vector<std::tuple<std::wstring, std::string, std::string,
    std::string, std::uint32_t, std::string, std::string>>
StructureSignature(
    const std::vector<LuaWidgetAccessibilitySnapshot>& snapshots)
{
    std::vector<std::tuple<std::wstring, std::string, std::string,
        std::string, std::uint32_t, std::string, std::string>> result;
    for (const auto& widget : snapshots)
    {
        result.emplace_back(widget.widgetId, std::string{},
            std::string{}, widget.packageId, 0u,
            std::string{}, std::string{});
        for (const auto& node : widget.nodes)
        {
            std::string parent;
            if (node.parentIndex != AccessibilityNode::NoParent &&
                node.parentIndex < widget.nodes.size())
                parent = widget.nodes[node.parentIndex].semanticId;
            result.emplace_back(widget.widgetId, node.semanticId,
                std::move(parent), node.controlType,
                static_cast<std::uint32_t>(node.patterns),
                node.labelledBySemanticId,
                node.describedBySemanticId);
        }
    }
    return result;
}

bool SameRect(const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}

bool SameRect(const snowdesktop::widget_runtime::ViewRect& left,
    const snowdesktop::widget_runtime::ViewRect& right) noexcept
{
    return left.x == right.x && left.y == right.y &&
        left.width == right.width && left.height == right.height;
}

void AddNodeChange(std::vector<WidgetAccessibilityChange>& changes,
    WidgetAccessibilityChangeKind kind, const NodeKey& key)
{
    changes.push_back({ kind, WidgetAccessibilityElementKind::Node,
        key.widgetId, key.semanticId });
}

double ScrollMaximum(const AccessibilityNode& node) noexcept
{
    return std::max(0.0, static_cast<double>(
        node.scrollContentExtent - node.scrollViewportExtent));
}

bool Scrollable(const AccessibilityNode& node) noexcept
{
    return ScrollMaximum(node) > 0.0;
}

double ScrollPercent(const AccessibilityNode& node) noexcept
{
    const double maximum = ScrollMaximum(node);
    return maximum > 0.0
        ? std::clamp(static_cast<double>(node.scrollOffset) /
                maximum * 100.0, 0.0, 100.0)
        : -1.0;
}

double ScrollViewSize(const AccessibilityNode& node) noexcept
{
    return node.scrollContentExtent > 0.0f
        ? std::clamp(static_cast<double>(node.scrollViewportExtent) /
                static_cast<double>(node.scrollContentExtent) * 100.0,
            0.0, 100.0)
        : 100.0;
}
}

std::vector<WidgetAccessibilityChange> DiffWidgetAccessibilitySnapshots(
    const std::vector<LuaWidgetAccessibilitySnapshot>& previous,
    const std::vector<LuaWidgetAccessibilitySnapshot>& current)
{
    std::vector<WidgetAccessibilityChange> changes;
    if (StructureSignature(previous) != StructureSignature(current))
        changes.push_back({ WidgetAccessibilityChangeKind::Structure,
            WidgetAccessibilityElementKind::Root, {}, {} });

    std::map<std::wstring, const LuaWidgetAccessibilitySnapshot*>
        previousWidgets;
    for (const auto& widget : previous)
        previousWidgets.emplace(widget.widgetId, &widget);
    for (const auto& widget : current)
    {
        const auto old = previousWidgets.find(widget.widgetId);
        if (old == previousWidgets.end()) continue;
        if (old->second->name != widget.name)
            changes.push_back({ WidgetAccessibilityChangeKind::Name,
                WidgetAccessibilityElementKind::Widget,
                widget.widgetId, {} });
        if (!SameRect(old->second->bounds, widget.bounds))
            changes.push_back({ WidgetAccessibilityChangeKind::Bounds,
                WidgetAccessibilityElementKind::Widget,
                widget.widgetId, {} });
    }

    const auto oldNodes = IndexNodes(previous);
    const auto newNodes = IndexNodes(current);
    std::optional<NodeKey> oldFocus;
    std::optional<NodeKey> newFocus;
    for (const auto& [key, entry] : oldNodes)
        if (entry.node->focused) oldFocus = key;
    for (const auto& [key, entry] : newNodes)
        if (entry.node->focused) newFocus = key;
    if (oldFocus != newFocus)
    {
        if (newFocus)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Focus, *newFocus);
        else
            changes.push_back({ WidgetAccessibilityChangeKind::Focus,
                WidgetAccessibilityElementKind::Root, {}, {} });
    }

    for (const auto& [key, currentEntry] : newNodes)
    {
        const auto previousEntry = oldNodes.find(key);
        if (previousEntry == oldNodes.end()) continue;
        const auto& old = *previousEntry->second.node;
        const auto& now = *currentEntry.node;
        if (old.name != now.name)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Name, key);
        if (old.role != now.role)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::AriaRole, key);
        if (old.helpText != now.helpText)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::HelpText, key);
        if (old.valueText != now.valueText &&
            !snowdesktop::widget_runtime::HasViewAccessibilityPattern(
                now.patterns, snowdesktop::widget_runtime::
                    ViewAccessibilityPattern::Value))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::ItemStatus, key);
        if (old.headingLevel != now.headingLevel)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::HeadingLevel, key);
        if (old.live != now.live)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::LiveSetting, key);
        if (old.positionInSet != now.positionInSet)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::PositionInSet, key);
        if (old.setSize != now.setSize)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::SizeOfSet, key);
        if (now.live != snowdesktop::widget_runtime::
                AccessibilityLive::Off &&
            (old.name != now.name || old.valueText != now.valueText ||
                old.helpText != now.helpText))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::LiveRegion, key);
        if (old.enabled != now.enabled)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Enabled, key);
        if (old.required != now.required)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Required, key);
        if (old.offscreen != now.offscreen)
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Offscreen, key);
        if (old.checked != now.checked &&
            snowdesktop::widget_runtime::HasViewAccessibilityPattern(
                now.patterns, snowdesktop::widget_runtime::
                    ViewAccessibilityPattern::Toggle))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Toggle, key);
        if (old.checked != now.checked &&
            snowdesktop::widget_runtime::HasViewAccessibilityPattern(
                now.patterns, snowdesktop::widget_runtime::
                    ViewAccessibilityPattern::SelectionItem))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::SelectionItem, key);
        if (old.value != now.value &&
            snowdesktop::widget_runtime::HasViewAccessibilityPattern(
                now.patterns, snowdesktop::widget_runtime::
                    ViewAccessibilityPattern::RangeValue))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::RangeValue, key);
        if (old.valueText != now.valueText &&
            snowdesktop::widget_runtime::HasViewAccessibilityPattern(
                now.patterns, snowdesktop::widget_runtime::
                    ViewAccessibilityPattern::Value))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Value, key);
        if (old.expanded != now.expanded &&
            snowdesktop::widget_runtime::HasViewAccessibilityPattern(
                now.patterns, snowdesktop::widget_runtime::
                    ViewAccessibilityPattern::ExpandCollapse))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::ExpandCollapse, key);
        if (!SameRect(old.bounds, now.bounds))
            AddNodeChange(changes,
                WidgetAccessibilityChangeKind::Bounds, key);
        if (snowdesktop::widget_runtime::HasViewAccessibilityPattern(
                now.patterns, snowdesktop::widget_runtime::
                    ViewAccessibilityPattern::Scroll))
        {
            if (old.scrollHorizontal != now.scrollHorizontal ||
                (now.scrollHorizontal &&
                    ScrollPercent(old) != ScrollPercent(now)))
                AddNodeChange(changes,
                    WidgetAccessibilityChangeKind::
                        HorizontalScrollPercent, key);
            if (old.scrollHorizontal != now.scrollHorizontal ||
                (now.scrollHorizontal &&
                    ScrollViewSize(old) != ScrollViewSize(now)))
                AddNodeChange(changes,
                    WidgetAccessibilityChangeKind::HorizontalViewSize,
                    key);
            if (old.scrollHorizontal != now.scrollHorizontal ||
                (now.scrollHorizontal &&
                    Scrollable(old) != Scrollable(now)))
                AddNodeChange(changes,
                    WidgetAccessibilityChangeKind::
                        HorizontallyScrollable, key);
            if (old.scrollHorizontal != now.scrollHorizontal ||
                (!now.scrollHorizontal &&
                    ScrollPercent(old) != ScrollPercent(now)))
                AddNodeChange(changes,
                    WidgetAccessibilityChangeKind::
                        VerticalScrollPercent, key);
            if (old.scrollHorizontal != now.scrollHorizontal ||
                (!now.scrollHorizontal &&
                    ScrollViewSize(old) != ScrollViewSize(now)))
                AddNodeChange(changes,
                    WidgetAccessibilityChangeKind::VerticalViewSize,
                    key);
            if (old.scrollHorizontal != now.scrollHorizontal ||
                (!now.scrollHorizontal &&
                    Scrollable(old) != Scrollable(now)))
                AddNodeChange(changes,
                    WidgetAccessibilityChangeKind::
                        VerticallyScrollable, key);
        }
    }
    return changes;
}
}
