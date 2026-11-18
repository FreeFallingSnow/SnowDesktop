#include "widget_view_tree.h"
#include "widget_view_geometry.h"
#include "widget_view_node_rules.h"
#include "widget/api/widget_view_contract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
namespace
{
using detail::ApplyTransform;
using detail::IntersectRects;
using detail::IsCheckControlNode;
using detail::IsCollectionContainer;
using detail::IsInputNode;
using detail::IsNodeKeyboardFocusable;
using detail::IsScrollContainer;

bool IsButtonNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Button ||
        type == ViewNodeType::IconButton;
}

const char* DefaultAccessibilityRole(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract ? contract->defaultAccessibilityRole.data() : "";
}

void ApplyNodeFocusPolicy(const ViewNode& node,
    InteractionRegion& region,
    bool includeAccessMetadata = true) noexcept
{
    region.focusable = IsNodeKeyboardFocusable(node);
    region.tabIndex = node.tabIndex.value_or(0);
    if (includeAccessMetadata)
    {
        region.accessKey = node.accessKey;
        region.acceleratorText = node.acceleratorText;
    }
}

bool Overlaps(const ViewRect& rect, const ViewRect& clip) noexcept
{
    return rect.x < clip.x + clip.width &&
        rect.x + rect.width > clip.x &&
        rect.y < clip.y + clip.height &&
        rect.y + rect.height > clip.y;
}

bool CollectRegions(const ViewNode& node,
    std::vector<InteractionRegion>& regions,
    const std::optional<ViewRect>& inheritedClip, float viewportHeight,
    std::string& error)
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return true;
    if (node.type == ViewNodeType::MonthCalendar)
    {
        std::array<ViewMonthCalendarCell, 42> cells;
        if (!BuildViewMonthCalendarCells(node, cells, error)) return false;
        std::map<std::string, InteractionAction, std::less<>> surfaceEvents;
        for (const auto& [name, action] : node.events)
            if (name != "change") surfaceEvents.emplace(name, action);
        if (!surfaceEvents.empty() || !node.tooltip.empty())
        {
            if (regions.size() >=
                WidgetInteractionRegions::kMaximumRegions)
            {
                error = "view interaction region limit exceeded (256)";
                return false;
            }
            InteractionRegion surface;
            surface.key = node.key;
            surface.shape.type = node.style.cornerRadius.value_or(0.0f) >
                    0.0f
                ? InteractionShapeType::RoundedRect
                : InteractionShapeType::Rect;
            surface.shape.x = node.frame.x;
            surface.shape.y = node.frame.y;
            surface.shape.width = node.frame.width;
            surface.shape.height = node.frame.height;
            surface.shape.radius = node.style.cornerRadius.value_or(0.0f);
            if (inheritedClip)
                surface.clip = InteractionClipRect{ inheritedClip->x,
                    inheritedClip->y, inheritedClip->width,
                    inheritedClip->height };
            surface.events = std::move(surfaceEvents);
            surface.tooltipTitle = node.tooltipTitle;
            surface.tooltip = node.tooltip;
            surface.accessibilityRole = node.accessibilityRole.empty()
                ? "grid" : node.accessibilityRole;
            surface.accessibilityLabel = node.accessibilityLabel;
            surface.capturePointer = node.capturePointer;
            surface.enabled = node.enabled;
            surface.focusable = false;
            regions.push_back(std::move(surface));
        }
        for (std::size_t index = 0; index < cells.size(); ++index)
        {
            const auto& cell = cells[index];
            if (!cell.currentMonth && !node.showAdjacentDates) continue;
            const ViewRect frame = ViewMonthCalendarCellFrame(node, index);
            if (regions.size() >=
                WidgetInteractionRegions::kMaximumRegions)
            {
                error = "view interaction region limit exceeded (256)";
                return false;
            }
            if (frame.width <= 0.0f || frame.height <= 0.0f)
            {
                error = "monthCalendar cell has an empty layout";
                return false;
            }
            InteractionRegion region;
            region.key = node.key + "/" + cell.date;
            region.shape.type = InteractionShapeType::Rect;
            region.shape.x = frame.x;
            region.shape.y = frame.y;
            region.shape.width = frame.width;
            region.shape.height = frame.height;
            if (inheritedClip)
                region.clip = InteractionClipRect{ inheritedClip->x,
                    inheritedClip->y, inheritedClip->width,
                    inheritedClip->height };
            region.cursor = node.cursor.empty() ? "hand" : node.cursor;
            region.tooltipTitle = node.tooltipTitle;
            region.tooltip = node.tooltip;
            region.events = node.events;
            region.controlKind = InteractionControlKind::Radio;
            region.checked = cell.selected;
            region.currentSelection = node.calendarSelectedDate;
            region.proposedSelection = cell.date;
            region.accessibilityRole = "gridcell";
            region.accessibilityLabel = cell.date;
            region.capturePointer = node.capturePointer;
            region.enabled = node.enabled;
            ApplyNodeFocusPolicy(node, region, false);
            regions.push_back(std::move(region));
        }
        return true;
    }
    if (node.type == ViewNodeType::RadioGroup)
    {
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            const auto& option = node.options[index];
            const ViewRect frame = ViewRadioOptionFrame(node, index);
            if (regions.size() >=
                WidgetInteractionRegions::kMaximumRegions)
            {
                error = "view interaction region limit exceeded (256)";
                return false;
            }
            if (frame.width <= 0.0f || frame.height <= 0.0f)
            {
                error = "interactive radio option has an empty layout: " +
                    node.key + "/" + option.key;
                return false;
            }
            InteractionRegion region;
            region.key = node.key + "/" + option.key;
            region.shape.type = node.style.cornerRadius.value_or(0.0f) > 0.0f
                ? InteractionShapeType::RoundedRect
                : InteractionShapeType::Rect;
            region.shape.x = frame.x;
            region.shape.y = frame.y;
            region.shape.width = frame.width;
            region.shape.height = frame.height;
            region.shape.radius = node.style.cornerRadius.value_or(0.0f);
            if (inheritedClip)
                region.clip = InteractionClipRect{ inheritedClip->x,
                    inheritedClip->y, inheritedClip->width,
                    inheritedClip->height };
            region.cursor = node.cursor.empty() ? "hand" : node.cursor;
            region.tooltipTitle = node.tooltipTitle;
            region.tooltip = node.tooltip;
            region.events = node.events;
            region.controlKind = InteractionControlKind::Radio;
            region.checked = option.value == node.selectedValue;
            region.currentSelection = node.selectedValue;
            region.proposedSelection = option.value;
            region.accessibilityRole = "radio";
            region.accessibilityLabel = option.label;
            region.capturePointer = node.capturePointer;
            region.enabled = node.enabled && option.enabled;
            ApplyNodeFocusPolicy(node, region, false);
            regions.push_back(std::move(region));
        }
        return true;
    }
    if (IsInputNode(node.type))
    {
        if (regions.size() >=
            WidgetInteractionRegions::kMaximumRegions)
        {
            error = "view interaction region limit exceeded (256)";
            return false;
        }
        InteractionRegion region;
        region.key = node.key;
        region.shape.type = node.style.cornerRadius.value_or(0.0f) > 0.0f
            ? InteractionShapeType::RoundedRect
            : InteractionShapeType::Rect;
        region.shape.x = node.frame.x;
        region.shape.y = node.frame.y;
        region.shape.width = node.frame.width;
        region.shape.height = node.frame.height;
        region.shape.radius = node.style.cornerRadius.value_or(0.0f);
        if (inheritedClip)
            region.clip = InteractionClipRect{ inheritedClip->x,
                inheritedClip->y, inheritedClip->width,
                inheritedClip->height };
        region.cursor = node.cursor.empty() ? "text" : node.cursor;
        region.tooltipTitle = node.tooltipTitle;
        region.tooltip = node.tooltip;
        for (const auto& [name, action] : node.events)
            if (name != "change" && name != "focus" &&
                name != "blur" && name != "submit" &&
                name != "selectionChange")
                region.events.emplace(name, action);
        region.accessibilityRole = node.accessibilityRole.empty()
            ? DefaultAccessibilityRole(node.type)
            : node.accessibilityRole;
        region.accessibilityLabel = node.accessibilityLabel;
        region.enabled = node.enabled;
        ApplyNodeFocusPolicy(node, region);
        regions.push_back(std::move(region));
        return true;
    }
    if (node.type == ViewNodeType::Select)
    {
        if (regions.size() >= WidgetInteractionRegions::kMaximumRegions)
        {
            error = "view interaction region limit exceeded (256)";
            return false;
        }
        InteractionRegion trigger;
        trigger.key = node.key;
        trigger.shape.type = node.style.cornerRadius.value_or(0.0f) > 0.0f
            ? InteractionShapeType::RoundedRect
            : InteractionShapeType::Rect;
        trigger.shape.x = node.frame.x;
        trigger.shape.y = node.frame.y;
        trigger.shape.width = node.frame.width;
        trigger.shape.height = node.frame.height;
        trigger.shape.radius = node.style.cornerRadius.value_or(0.0f);
        if (inheritedClip)
            trigger.clip = InteractionClipRect{ inheritedClip->x,
                inheritedClip->y, inheritedClip->width,
                inheritedClip->height };
        trigger.cursor = node.cursor.empty() ? "hand" : node.cursor;
        trigger.tooltipTitle = node.tooltipTitle;
        trigger.tooltip = node.tooltip;
        for (const auto& [name, action] : node.events)
            if (name != "change") trigger.events.emplace(name, action);
        trigger.accessibilityRole = node.accessibilityRole.empty()
            ? "combobox" : node.accessibilityRole;
        trigger.accessibilityLabel = node.accessibilityLabel;
        trigger.hasExpandedProposal = true;
        trigger.expanded = node.expanded;
        trigger.enabled = node.enabled;
        ApplyNodeFocusPolicy(node, trigger);
        regions.push_back(std::move(trigger));
        return true;
    }
    const bool hasDirectRegionEvent = std::any_of(node.events.begin(),
        node.events.end(), [&node](const auto& entry) {
            return !IsCollectionContainer(node.type) ||
                entry.first != "change";
        });
    if ((hasDirectRegionEvent || !node.tooltip.empty() ||
            IsNodeKeyboardFocusable(node) ||
            IsButtonNode(node.type) ||
            node.type == ViewNodeType::ListItem ||
            node.type == ViewNodeType::SlotItem))
    {
        if (regions.size() >= WidgetInteractionRegions::kMaximumRegions)
        {
            error = "view interaction region limit exceeded (256)";
            return false;
        }
        if (node.frame.width <= 0.0f || node.frame.height <= 0.0f)
        {
            error = "interactive view node has an empty layout: " + node.key;
            return false;
        }
        InteractionRegion region;
        region.key = node.key;
        if (inheritedClip)
            region.clip = InteractionClipRect{ inheritedClip->x,
                inheritedClip->y, inheritedClip->width,
                inheritedClip->height };
        if (node.type == ViewNodeType::Shape &&
            node.shapeKind == ViewShapeKind::Circle)
        {
            region.shape.type = InteractionShapeType::Circle;
            region.shape.x = node.frame.x + node.frame.width * 0.5f;
            region.shape.y = node.frame.y + node.frame.height * 0.5f;
            region.shape.radius = std::min(
                node.frame.width, node.frame.height) * 0.5f;
        }
        else
        {
            region.shape.type =
                node.style.cornerRadius.value_or(0.0f) > 0.0f
                ? InteractionShapeType::RoundedRect
                : InteractionShapeType::Rect;
            region.shape.x = node.frame.x;
            region.shape.y = node.frame.y;
            region.shape.width = node.frame.width;
            region.shape.height = node.frame.height;
            region.shape.radius =
                node.style.cornerRadius.value_or(0.0f);
        }
        region.cursor = node.cursor.empty() &&
            (IsButtonNode(node.type) || IsCheckControlNode(node.type) ||
                node.type == ViewNodeType::Link ||
                node.type == ViewNodeType::Slider ||
                (node.type == ViewNodeType::ListItem &&
                    node.inheritedSelectionMode !=
                        ViewSelectionMode::None) ||
                node.events.contains("click"))
            ? "hand" : node.cursor;
        region.tooltipTitle = node.tooltipTitle;
        region.tooltip = node.tooltip;
        for (const auto& [name, action] : node.events)
            if (!IsCollectionContainer(node.type) || name != "change")
                region.events.emplace(name, action);
        if (node.type == ViewNodeType::Toggle)
            region.controlKind = InteractionControlKind::Toggle;
        else if (node.type == ViewNodeType::Checkbox)
            region.controlKind = InteractionControlKind::Checkbox;
        else if (node.type == ViewNodeType::Slider)
        {
            region.controlKind = InteractionControlKind::Slider;
            region.controlValue = node.value;
            region.minimum = node.minimum;
            region.maximum = node.maximum;
            region.step = node.step;
            region.vertical = node.orientation == ViewOrientation::Vertical;
            const float thumbRadius = std::min(8.0f, std::max(3.0f,
                std::min(node.frame.width, node.frame.height) * 0.3f));
            const ViewRect content = ViewNodeContentRect(node);
            const float mainLength = region.vertical
                ? content.height : content.width;
            const float radiusInset = std::min(thumbRadius,
                std::max(0.0f, mainLength * 0.5f));
            region.controlStart = (region.vertical
                ? content.y : content.x) + radiusInset;
            region.controlLength = std::max(0.0f,
                mainLength - radiusInset * 2.0f);
        }
        else if (node.type == ViewNodeType::ListItem &&
            node.inheritedSelectionMode != ViewSelectionMode::None)
        {
            region.controlKind = node.inheritedSelectionMode ==
                    ViewSelectionMode::Single
                ? InteractionControlKind::SelectionSingle
                : InteractionControlKind::SelectionMultiple;
            region.currentSelectedKeys = node.inheritedSelectedKeys;
            region.proposedSelectedKey = node.key;
            if (node.inheritedSelectionChangeAction)
                region.events.insert_or_assign("change",
                    *node.inheritedSelectionChangeAction);
        }
        region.checked = node.type == ViewNodeType::ListItem &&
                node.inheritedSelectionMode != ViewSelectionMode::None
            ? node.selected : node.checked;
        region.indeterminate = node.indeterminate;
        region.accessibilityRole = node.accessibilityRole.empty()
            ? DefaultAccessibilityRole(node.type)
            : node.accessibilityRole;
        region.accessibilityLabel = node.accessibilityLabel.empty()
            ? node.text : node.accessibilityLabel;
        region.capturePointer = node.capturePointer;
        region.enabled = node.enabled;
        ApplyNodeFocusPolicy(node, region);
        regions.push_back(std::move(region));
    }
    std::optional<ViewRect> childClip = inheritedClip;
    if (node.clipFrame)
    {
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    }
    for (const ViewNode* child : ViewChildrenInPaintOrder(node))
        if (!CollectRegions(*child, regions, childClip,
                viewportHeight, error)) return false;
    return true;
}

bool CollectSelectOptions(const ViewNode& node,
    std::vector<InteractionRegion>& regions,
    const std::optional<ViewRect>& inheritedClip, float viewportHeight,
    std::string& error)
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return true;
    if (node.type == ViewNodeType::Select && node.expanded)
    {
        const ViewRect popup = ViewSelectPopupFrame(node, viewportHeight);
        if (popup.width <= 0.0f || popup.height <= 0.0f) return true;
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            const auto& option = node.options[index];
            const ViewRect frame = ViewSelectOptionFrame(
                node, index, viewportHeight);
            if (regions.size() >=
                WidgetInteractionRegions::kMaximumRegions)
            {
                error = "view interaction region limit exceeded (256)";
                return false;
            }
            InteractionRegion region;
            region.key = node.key + "/" + option.key;
            region.shape.type = InteractionShapeType::Rect;
            region.shape.x = frame.x;
            region.shape.y = frame.y;
            region.shape.width = frame.width;
            region.shape.height = frame.height;
            region.clip = InteractionClipRect{ popup.x, popup.y,
                popup.width, popup.height };
            region.cursor = "hand";
            region.tooltipTitle = node.tooltipTitle;
            region.tooltip = node.tooltip;
            for (const auto& [name, action] : node.events)
                if (name != "click") region.events.emplace(name, action);
            region.controlKind = InteractionControlKind::Radio;
            region.checked = option.value == node.selectedValue;
            region.currentSelection = node.selectedValue;
            region.proposedSelection = option.value;
            region.keyboardNavigationGroup = node.key;
            region.accessibilityRole = "option";
            region.accessibilityLabel = option.label;
            region.enabled = node.enabled && option.enabled;
            ApplyNodeFocusPolicy(node, region, false);
            regions.push_back(std::move(region));
        }
        return true;
    }
    std::optional<ViewRect> childClip = inheritedClip;
    if (node.clipFrame)
    {
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    }
    for (const ViewNode* child : ViewChildrenInPaintOrder(node))
        if (!CollectSelectOptions(*child, regions, childClip,
                viewportHeight, error)) return false;
    return true;
}

bool CollectInputs(const ViewNode& node,
    std::vector<ViewInputControl>& controls,
    const std::optional<ViewRect>& inheritedClip, std::string& error)
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return true;
    if (IsInputNode(node.type))
    {
        if (controls.size() >= 128)
        {
            error = "view input control limit exceeded (128)";
            return false;
        }
        ViewInputControl control;
        control.type = node.type;
        control.key = node.key;
        control.value = node.type == ViewNodeType::NumberInput
            ? std::to_string(node.value) : node.inputValue;
        if (node.type == ViewNodeType::NumberInput)
        {
            while (control.value.size() > 1 && control.value.back() == '0')
                control.value.pop_back();
            if (!control.value.empty() && control.value.back() == '.')
                control.value.pop_back();
        }
        control.placeholder = node.placeholder;
        control.frame = node.frame;
        control.clip = inheritedClip;
        control.fontSize = node.fontSize;
        control.padding = node.padding;
        control.enabled = node.enabled;
        control.focusable = IsNodeKeyboardFocusable(node);
        control.readOnly = node.readOnly;
        control.selectAll = node.selectAll;
        control.selection = node.textSelection;
        control.liveUpdate = node.liveUpdate;
        control.maximumUtf8Bytes = node.maximumUtf8Bytes;
        control.minimum = node.minimum;
        control.maximum = node.maximum;
        control.step = node.step;
        if (const auto action = node.events.find("change");
            action != node.events.end()) control.changeAction = action->second;
        if (const auto action = node.events.find("selectionChange");
            action != node.events.end())
            control.selectionChangeAction = action->second;
        if (const auto action = node.events.find("focus");
            action != node.events.end()) control.focusAction = action->second;
        if (const auto action = node.events.find("blur");
            action != node.events.end()) control.blurAction = action->second;
        if (const auto action = node.events.find("submit");
            action != node.events.end()) control.submitAction = action->second;
        controls.push_back(std::move(control));
        return true;
    }
    std::optional<ViewRect> childClip = inheritedClip;
    if (node.clipFrame)
    {
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    }
    for (const ViewNode* child : ViewChildrenInPaintOrder(node))
        if (!CollectInputs(*child, controls, childClip, error)) return false;
    return true;
}

float TransformScaleX(const ViewResolvedTransform& transform) noexcept
{
    return std::hypot(transform.m11, transform.m12);
}

float TransformScaleY(const ViewResolvedTransform& transform) noexcept
{
    return std::hypot(transform.m21, transform.m22);
}

bool InteractionRegionOverlapsClip(
    const InteractionRegion& region) noexcept
{
    if (!region.clip) return true;
    const ViewRect clip{ region.clip->x, region.clip->y,
        region.clip->width, region.clip->height };
    const auto overlapsShape = [&clip](const InteractionShape& shape) {
        const ViewRect bounds = shape.type == InteractionShapeType::Circle
            ? ViewRect{ shape.x - shape.radius, shape.y - shape.radius,
                shape.radius * 2.0f, shape.radius * 2.0f }
            : ViewRect{ shape.x, shape.y, shape.width, shape.height };
        return Overlaps(bounds, clip);
    };
    if (!region.hitFragments.empty())
        return std::any_of(region.hitFragments.begin(),
            region.hitFragments.end(), overlapsShape);
    return overlapsShape(region.shape);
}

std::vector<std::string> KeyboardScrollAncestors(
    const ViewNode& root, std::string_view key)
{
    std::vector<std::string> path, result;
    std::size_t matchedLength = 0;
    const auto visit = [&](const auto& self, const ViewNode& node) -> void {
        if (!node.visible || node.visibility == ViewVisibility::Hidden) return;
        const bool generated = key.size() > node.key.size() &&
            key.starts_with(node.key) && key[node.key.size()] == '/';
        if ((key == node.key || generated) && node.key.size() >= matchedLength)
        {
            matchedLength = node.key.size();
            result = path;
            if (generated && node.type == ViewNodeType::Select && node.expanded)
                result.push_back(node.key);
        }
        const bool scroll = IsScrollContainer(node.type);
        if (scroll) path.push_back(node.key);
        for (const auto& child : node.children) self(self, child);
        if (scroll) path.pop_back();
    };
    visit(visit, root);
    return result;
}
}

void ApplyViewTransform(const ViewNode& root,
    InteractionRegion& region, bool preserveClip) noexcept
{
    const auto transform = ResolveViewTransformForKey(root, region.key);
    if (preserveClip && region.clip)
    {
        const ViewRect clip = ApplyViewTransform(
            { region.clip->x, region.clip->y,
                region.clip->width, region.clip->height }, transform);
        region.clip = InteractionClipRect{
            clip.x, clip.y, clip.width, clip.height };
    }
    else if (const auto clip = ResolveViewClipForKey(
            root, region.key, false))
    {
        region.clip = InteractionClipRect{
            clip->x, clip->y, clip->width, clip->height };
    }
    else region.clip.reset();
    constexpr float epsilon = 0.000001f;
    if (std::abs(transform.m11 - 1.0f) <= epsilon &&
        std::abs(transform.m22 - 1.0f) <= epsilon &&
        std::abs(transform.m12) <= epsilon &&
        std::abs(transform.m21) <= epsilon &&
        std::abs(transform.dx) <= epsilon &&
        std::abs(transform.dy) <= epsilon)
        return;
    region.localHitShape = region.shape;
    region.localHitFragments = region.hitFragments;
    region.hitTransform = InteractionAffineTransform{
        transform.m11, transform.m12, transform.m21, transform.m22,
        transform.dx, transform.dy };
    const auto applyShape = [&transform](InteractionShape& shape) {
        if (shape.type == InteractionShapeType::Circle)
        {
            const auto center = ApplyTransform(shape.x, shape.y, transform);
            const float extentX = shape.radius * std::hypot(
                transform.m11, transform.m21);
            const float extentY = shape.radius * std::hypot(
                transform.m12, transform.m22);
            shape.type = InteractionShapeType::Rect;
            shape.x = center.x - extentX;
            shape.y = center.y - extentY;
            shape.width = extentX * 2.0f;
            shape.height = extentY * 2.0f;
            shape.radius = 0.0f;
            return;
        }
        const ViewRect bounds = ApplyViewTransform(
            { shape.x, shape.y, shape.width, shape.height }, transform);
        shape.type = InteractionShapeType::Rect;
        shape.x = bounds.x;
        shape.y = bounds.y;
        shape.width = bounds.width;
        shape.height = bounds.height;
        shape.radius = 0.0f;
    };
    applyShape(region.shape);
    for (auto& fragment : region.hitFragments)
        applyShape(fragment);
    if (region.controlKind == InteractionControlKind::Slider &&
        region.controlLength > 0.0f)
    {
        const float cross = region.vertical
            ? region.localHitShape->x + region.localHitShape->width * 0.5f
            : region.localHitShape->y + region.localHitShape->height * 0.5f;
        const auto start = region.vertical
            ? ApplyTransform(cross, region.controlStart, transform)
            : ApplyTransform(region.controlStart, cross, transform);
        const auto end = region.vertical
            ? ApplyTransform(cross,
                region.controlStart + region.controlLength, transform)
            : ApplyTransform(
                region.controlStart + region.controlLength, cross, transform);
        region.hasControlAxis = true;
        region.controlStartX = start.x;
        region.controlStartY = start.y;
        region.controlEndX = end.x;
        region.controlEndY = end.y;
    }
}

std::vector<std::pair<std::string, float>> ComputeViewKeyboardScrollOffsets(
    const ViewNode& root, const InteractionRegion& target)
{
    std::vector<std::pair<std::string, float>> offsets;
    const bool circle = target.shape.type == InteractionShapeType::Circle;
    ViewRect bounds{circle ? target.shape.x - target.shape.radius : target.shape.x,
        circle ? target.shape.y - target.shape.radius : target.shape.y,
        circle ? target.shape.radius * 2.0f : target.shape.width,
        circle ? target.shape.radius * 2.0f : target.shape.height};
    const auto find = [&](const auto& self, const ViewNode& node,
                          std::string_view key) -> const ViewNode* {
        if (node.key == key) return &node;
        for (const auto& child : node.children)
            if (const auto* match = self(self, child, key)) return match;
        return nullptr;
    };
    for (auto id = target.keyboardScrollAncestors.rbegin();
         id != target.keyboardScrollAncestors.rend(); ++id)
    {
        const auto* node = find(find, root, *id);
        if (!node) continue;
        const bool select = node->type == ViewNodeType::Select;
        const bool horizontal = !select && node->orientation == ViewOrientation::Horizontal;
        const auto transform = ResolveViewTransformForKey(root, *id);
        const auto viewport = ApplyViewTransform(select ?
            ViewSelectPopupFrame(*node, root.frame.height) :
            node->clipFrame.value_or(node->frame), transform);
        const float scale = horizontal ? transform.m11 : transform.m22;
        if (viewport.width <= 0.0f || viewport.height <= 0.0f || std::abs(scale) < 0.000001f) continue;
        const float start = horizontal ? bounds.x : bounds.y;
        const float end = start + (horizontal ? bounds.width : bounds.height);
        const float viewportStart = horizontal ? viewport.x : viewport.y;
        const float viewportEnd = viewportStart + (horizontal ? viewport.width : viewport.height);
        const float delta = start < viewportStart ? start - viewportStart :
            end > viewportEnd ? std::min(start - viewportStart, end - viewportEnd) : 0.0f;
        const float maximum = std::max(0.0f, node->scrollContentExtent - node->scrollViewportExtent);
        const float next = std::clamp(node->scrollOffset + delta / scale, 0.0f, maximum);
        const float change = next - node->scrollOffset;
        if (std::abs(change) < 0.5f) continue;
        offsets.emplace_back(*id, next);
        bounds.x -= change * (horizontal ? transform.m11 : transform.m21);
        bounds.y -= change * (horizontal ? transform.m12 : transform.m22);
    }
    return offsets;
}

bool CollectViewInteractionRegions(const ViewNode& root,
    std::vector<InteractionRegion>& regions, std::string& error)
{
    error.clear();
    regions.clear();
    if (!CollectRegions(root, regions, std::nullopt,
            root.frame.height, error)) return false;
    const std::size_t optionStart = regions.size();
    if (!CollectSelectOptions(root, regions, std::nullopt,
            root.frame.height, error)) return false;
    if (regions.size() > WidgetInteractionRegions::kMaximumRegions)
    {
        error = "view interaction region limit exceeded (256)";
        regions.clear();
        return false;
    }
    for (std::size_t index = 0; index < regions.size(); ++index)
    {
        ApplyViewTransform(root, regions[index], index >= optionStart);
        regions[index].keyboardScrollAncestors = KeyboardScrollAncestors(root, regions[index].key);
    }
    std::erase_if(regions, [](const auto& region) {
        // Keep realized logical controls for keyboard navigation and scrolling.
        // Physical pointer hit testing still enforces their popup clip.
        const bool scrollable = !region.keyboardScrollAncestors.empty() &&
            region.focusable.value_or(false) && region.clip;
        return !scrollable && !InteractionRegionOverlapsClip(region);
    });
    return true;
}

bool CollectViewInputControls(const ViewNode& root,
    std::vector<ViewInputControl>& controls, std::string& error)
{
    error.clear();
    controls.clear();
    if (!CollectInputs(root, controls, std::nullopt, error)) return false;
    for (auto& control : controls)
    {
        const auto transform = ResolveViewTransformForKey(root, control.key);
        control.frame = ApplyViewTransform(control.frame, transform);
        control.clip = ResolveViewClipForKey(root, control.key, false);
        const float scaleX = TransformScaleX(transform);
        const float scaleY = TransformScaleY(transform);
        control.fontSize *= scaleY;
        control.padding.top *= scaleY;
        control.padding.right *= scaleX;
        control.padding.bottom *= scaleY;
        control.padding.left *= scaleX;
    }
    std::erase_if(controls, [&root](const auto& control) {
        const bool scrollable = control.focusable &&
            !KeyboardScrollAncestors(root, control.key).empty() && control.clip;
        return !scrollable && control.clip && !Overlaps(control.frame, *control.clip);
    });
    return true;
}
}
