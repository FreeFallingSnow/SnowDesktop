#include "widget_view_tree.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr float MaximumDimension = 100000.0f;

bool FiniteInRange(float value, float minimum, float maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool ValidateLength(const ViewLength& length) noexcept
{
    return length.kind != ViewLengthKind::Fixed ||
        FiniteInRange(length.value, 0.0f, MaximumDimension);
}

float TextIntrinsicWidth(const ViewNode& node) noexcept
{
    const float approximateGlyphs = static_cast<float>(
        std::min<std::size_t>(node.text.size(), 256));
    return std::max(node.fontSize, approximateGlyphs * node.fontSize * 0.55f) +
        node.padding * 2.0f;
}

bool IsIconNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Icon ||
        type == ViewNodeType::IconButton;
}

bool IsButtonNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Button ||
        type == ViewNodeType::IconButton;
}

bool IsCheckControlNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Toggle ||
        type == ViewNodeType::Checkbox;
}

bool IsDataSeriesNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Sparkline ||
        type == ViewNodeType::LineChart ||
        type == ViewNodeType::BarChart ||
        type == ViewNodeType::Waveform ||
        type == ViewNodeType::Spectrum;
}

bool IsLeafNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Text || type == ViewNodeType::Image ||
        IsButtonNode(type) || IsCheckControlNode(type) ||
        type == ViewNodeType::Icon || type == ViewNodeType::Shape ||
        type == ViewNodeType::Badge || type == ViewNodeType::Divider ||
        type == ViewNodeType::ProgressBar ||
        type == ViewNodeType::ProgressRing ||
        type == ViewNodeType::Meter ||
        IsDataSeriesNode(type) ||
        type == ViewNodeType::Spacer;
}

float IntrinsicWidth(const ViewNode& node);
float IntrinsicHeight(const ViewNode& node);

float IntrinsicWidth(const ViewNode& node)
{
    if (node.width.kind == ViewLengthKind::Fixed)
        return node.width.value;
    if (node.type == ViewNodeType::Text ||
        node.type == ViewNodeType::Badge ||
        node.type == ViewNodeType::Button)
        return TextIntrinsicWidth(node);
    if (node.type == ViewNodeType::Toggle)
        return TextIntrinsicWidth(node) + 44.0f;
    if (node.type == ViewNodeType::Checkbox)
        return TextIntrinsicWidth(node) + 26.0f;
    if (node.type == ViewNodeType::Image)
        return 48.0f + node.padding * 2.0f;
    if (IsIconNode(node.type))
        return node.fontSize * 1.4f + node.padding * 2.0f;
    if (node.type == ViewNodeType::ProgressRing)
        return 32.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Shape)
        return 8.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Divider)
        return (node.orientation == ViewOrientation::Vertical
            ? node.thickness : 24.0f) + node.padding * 2.0f;
    if (node.type == ViewNodeType::Meter)
        return 64.0f + node.padding * 2.0f;
    if (IsDataSeriesNode(node.type))
        return 64.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Spacer)
        return 0.0f;
    float result = 0.0f;
    if (node.type == ViewNodeType::Row)
    {
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += IntrinsicWidth(child);
            ++visible;
        }
        if (visible > 1)
            result += node.gap * static_cast<float>(visible - 1);
    }
    else
    {
        for (const auto& child : node.children)
            if (child.visible) result = std::max(result, IntrinsicWidth(child));
    }
    return result + node.padding * 2.0f;
}

float IntrinsicHeight(const ViewNode& node)
{
    if (node.height.kind == ViewLengthKind::Fixed)
        return node.height.value;
    if (node.type == ViewNodeType::Text)
        return node.fontSize * 1.4f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Badge)
        return std::max(20.0f, node.fontSize * 1.4f) +
            node.padding * 2.0f;
    if (node.type == ViewNodeType::Image)
        return 48.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Button)
        return std::max(32.0f, node.fontSize * 1.8f) +
            node.padding * 2.0f;
    if (node.type == ViewNodeType::Toggle ||
        node.type == ViewNodeType::Checkbox)
        return std::max(32.0f, node.fontSize * 1.8f) +
            node.padding * 2.0f;
    if (node.type == ViewNodeType::IconButton)
        return std::max(32.0f, node.fontSize * 1.4f) +
            node.padding * 2.0f;
    if (node.type == ViewNodeType::Icon)
        return node.fontSize * 1.4f + node.padding * 2.0f;
    if (node.type == ViewNodeType::ProgressBar ||
        node.type == ViewNodeType::Meter)
        return std::max(4.0f, node.thickness) + node.padding * 2.0f;
    if (node.type == ViewNodeType::ProgressRing)
        return 32.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Shape)
        return 8.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Divider)
        return (node.orientation == ViewOrientation::Horizontal
            ? node.thickness : 24.0f) + node.padding * 2.0f;
    if (IsDataSeriesNode(node.type))
        return 40.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Spacer)
        return 0.0f;
    float result = 0.0f;
    if (node.type == ViewNodeType::Column)
    {
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += IntrinsicHeight(child);
            ++visible;
        }
        if (visible > 1)
            result += node.gap * static_cast<float>(visible - 1);
    }
    else
    {
        for (const auto& child : node.children)
            if (child.visible) result = std::max(result, IntrinsicHeight(child));
    }
    return result + node.padding * 2.0f;
}

float ResolveCrossSize(const ViewLength& length, float intrinsic,
    float available, ViewAlignment alignment) noexcept
{
    if (length.kind == ViewLengthKind::Fixed)
        return std::min(length.value, available);
    if (length.kind == ViewLengthKind::Fill ||
        alignment == ViewAlignment::Stretch)
        return available;
    return std::min(intrinsic, available);
}

float AlignOffset(ViewAlignment alignment, float available,
    float size) noexcept
{
    if (alignment == ViewAlignment::Center)
        return std::max(0.0f, (available - size) * 0.5f);
    if (alignment == ViewAlignment::End)
        return std::max(0.0f, available - size);
    return 0.0f;
}

void LayoutNode(ViewNode& node, const ViewRect& frame);

void LayoutLinear(ViewNode& node, const ViewRect& content, bool horizontal)
{
    std::vector<ViewNode*> visible;
    visible.reserve(node.children.size());
    for (auto& child : node.children)
        if (child.visible) visible.push_back(&child);
    if (visible.empty()) return;

    const float availableMain = horizontal ? content.width : content.height;
    const float availableCross = horizontal ? content.height : content.width;
    const float gaps = node.gap * static_cast<float>(visible.size() - 1);
    float fixed = 0.0f;
    float flex = 0.0f;
    std::vector<float> mainSizes(visible.size(), 0.0f);
    for (std::size_t index = 0; index < visible.size(); ++index)
    {
        const auto& child = *visible[index];
        const ViewLength& mainLength = horizontal
            ? child.width : child.height;
        if (mainLength.kind == ViewLengthKind::Fill || child.flexGrow > 0.0f)
        {
            flex += std::max(1.0f, child.flexGrow);
            continue;
        }
        const float intrinsic = horizontal
            ? IntrinsicWidth(child) : IntrinsicHeight(child);
        mainSizes[index] = mainLength.kind == ViewLengthKind::Fixed
            ? mainLength.value : intrinsic;
        fixed += mainSizes[index];
    }
    const float remaining = std::max(0.0f, availableMain - fixed - gaps);
    if (flex > 0.0f)
    {
        for (std::size_t index = 0; index < visible.size(); ++index)
        {
            const auto& child = *visible[index];
            const ViewLength& mainLength = horizontal
                ? child.width : child.height;
            if (mainLength.kind == ViewLengthKind::Fill || child.flexGrow > 0.0f)
                mainSizes[index] = remaining *
                    (std::max(1.0f, child.flexGrow) / flex);
        }
    }

    float used = gaps;
    for (float value : mainSizes) used += value;
    float cursor = horizontal ? content.x : content.y;
    float dynamicGap = node.gap;
    if (node.justifyContent == ViewJustification::Center)
        cursor += std::max(0.0f, (availableMain - used) * 0.5f);
    else if (node.justifyContent == ViewJustification::End)
        cursor += std::max(0.0f, availableMain - used);
    else if (node.justifyContent == ViewJustification::SpaceBetween &&
        visible.size() > 1 && availableMain > used)
    {
        dynamicGap += (availableMain - used) /
            static_cast<float>(visible.size() - 1);
    }

    for (std::size_t index = 0; index < visible.size(); ++index)
    {
        ViewNode& child = *visible[index];
        const ViewLength& crossLength = horizontal
            ? child.height : child.width;
        const float intrinsicCross = horizontal
            ? IntrinsicHeight(child) : IntrinsicWidth(child);
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? node.alignItems : child.alignSelf;
        const float crossSize = ResolveCrossSize(
            crossLength, intrinsicCross, availableCross, alignment);
        const float crossOffset = AlignOffset(
            alignment, availableCross, crossSize);
        ViewRect childFrame;
        if (horizontal)
            childFrame = { cursor, content.y + crossOffset,
                mainSizes[index], crossSize };
        else
            childFrame = { content.x + crossOffset, cursor,
                crossSize, mainSizes[index] };
        LayoutNode(child, childFrame);
        cursor += mainSizes[index] + dynamicGap;
    }
}

void LayoutNode(ViewNode& node, const ViewRect& frame)
{
    node.frame = frame;
    const float inset = std::min(node.padding,
        std::max(0.0f, std::min(frame.width, frame.height) * 0.5f));
    const ViewRect content{
        frame.x + inset,
        frame.y + inset,
        std::max(0.0f, frame.width - inset * 2.0f),
        std::max(0.0f, frame.height - inset * 2.0f),
    };
    if (node.type == ViewNodeType::Row)
        LayoutLinear(node, content, true);
    else if (node.type == ViewNodeType::Column)
        LayoutLinear(node, content, false);
    else if (node.type == ViewNodeType::Box ||
        node.type == ViewNodeType::Stack)
    {
        for (auto& child : node.children)
        {
            if (!child.visible) continue;
            const ViewAlignment alignment =
                child.alignSelf == ViewAlignment::Auto
                ? ViewAlignment::Stretch : child.alignSelf;
            const float width = ResolveCrossSize(child.width,
                IntrinsicWidth(child), content.width, alignment);
            const float height = ResolveCrossSize(child.height,
                IntrinsicHeight(child), content.height, alignment);
            LayoutNode(child, {
                content.x + AlignOffset(alignment,
                    content.width, width),
                content.y + AlignOffset(alignment,
                    content.height, height),
                width, height });
        }
    }
}

bool ValidateNode(const ViewNode& node, std::size_t depth,
    std::size_t& nodes, std::size_t& textBytes,
    std::size_t& seriesPoints,
    std::unordered_set<std::string>& keys,
    std::unordered_set<std::string>& resources, std::string& error)
{
    if (++nodes > ViewTreeLimits::MaximumNodes)
    {
        error = "view tree node limit exceeded (512)";
        return false;
    }
    if (depth > ViewTreeLimits::MaximumDepth)
    {
        error = "view tree depth limit exceeded (32)";
        return false;
    }
    if (node.key.empty() || node.key.size() > 128)
    {
        error = "view node key must contain 1 to 128 bytes";
        return false;
    }
    if (!keys.insert(node.key).second)
    {
        error = "duplicate view node key: " + node.key;
        return false;
    }
    const auto validStyle = [](const ViewStyle& style) {
        return (!style.borderWidth || FiniteInRange(
                    *style.borderWidth, 0.0f, 4096.0f)) &&
            (!style.cornerRadius || FiniteInRange(
                    *style.cornerRadius, 0.0f, 4096.0f)) &&
            (!style.opacity || FiniteInRange(*style.opacity, 0.0f, 1.0f));
    };
    if (!ValidateLength(node.width) || !ValidateLength(node.height) ||
        !FiniteInRange(node.padding, 0.0f, 4096.0f) ||
        !FiniteInRange(node.gap, 0.0f, 4096.0f) ||
        !FiniteInRange(node.flexGrow, 0.0f, 1000.0f) ||
        !FiniteInRange(node.fontSize, 1.0f, 512.0f) ||
        !FiniteInRange(node.value, 0.0f, 1.0f) ||
        !FiniteInRange(node.thickness, 0.5f, 4096.0f) ||
        !FiniteInRange(node.trackOpacity, 0.0f, 1.0f) ||
        !FiniteInRange(node.fillOpacity, 0.0f, 1.0f) ||
        !validStyle(node.style) || !validStyle(node.hoverStyle) ||
        !validStyle(node.pressedStyle) ||
        !validStyle(node.checkedStyle))
    {
        error = "view node dimensions and typography must be finite and bounded";
        return false;
    }
    if (node.text.size() > ViewTreeLimits::MaximumTextBytes ||
        node.alt.size() > ViewTreeLimits::MaximumTextBytes ||
        textBytes + node.text.size() + node.alt.size() >
            ViewTreeLimits::MaximumTotalTextBytes)
    {
        error = "view tree text limit exceeded";
        return false;
    }
    textBytes += node.text.size() + node.alt.size();
    if (node.accessibilityRole.size() > 128 ||
        node.accessibilityLabel.size() >
            ViewTreeLimits::MaximumTextBytes ||
        textBytes + node.accessibilityLabel.size() >
            ViewTreeLimits::MaximumTotalTextBytes)
    {
        error = "view tree accessibility text limit exceeded";
        return false;
    }
    textBytes += node.accessibilityLabel.size();
    if (IsDataSeriesNode(node.type))
    {
        if (node.values.empty() ||
            node.values.size() > ViewTreeLimits::MaximumSeriesPoints ||
            seriesPoints > ViewTreeLimits::MaximumTotalSeriesPoints -
                node.values.size())
        {
            error = "view data-series point limit exceeded";
            return false;
        }
        seriesPoints += node.values.size();
        if (!std::all_of(node.values.begin(), node.values.end(),
                [](float value) {
                    return FiniteInRange(value, -1.0e9f, 1.0e9f);
                }))
        {
            error = "view data-series values must be finite and bounded";
            return false;
        }
        if (node.seriesMinimum.has_value() !=
                node.seriesMaximum.has_value() ||
            (node.seriesMinimum &&
                (!FiniteInRange(*node.seriesMinimum, -1.0e9f, 1.0e9f) ||
                    !FiniteInRange(*node.seriesMaximum,
                        -1.0e9f, 1.0e9f) ||
                    *node.seriesMinimum >= *node.seriesMaximum)))
        {
            error = "view data-series min must be less than max";
            return false;
        }
        if (node.accessibilityLabel.empty())
        {
            error = std::string(ViewNodeTypeName(node.type)) +
                " nodes require accessibility.label";
            return false;
        }
    }
    else if (!node.values.empty() || node.seriesMinimum ||
        node.seriesMaximum)
    {
        error = "only data-series nodes can retain series data";
        return false;
    }
    if (node.type == ViewNodeType::Image &&
        node.imageResourceName.empty())
    {
        error = "image nodes require an image resource handle";
        return false;
    }
    if (node.type != ViewNodeType::Image &&
        !node.imageResourceName.empty())
    {
        error = "only image nodes can retain an image resource";
        return false;
    }
    if (!node.fontResourceName.empty() &&
        node.type != ViewNodeType::Text &&
        node.type != ViewNodeType::Button &&
        node.type != ViewNodeType::Badge &&
        !IsCheckControlNode(node.type))
    {
        error = "only text, badge, button, toggle, and checkbox nodes can retain a font resource";
        return false;
    }
    if (!node.imageResourceName.empty())
        resources.insert("image:" + node.imageResourceName);
    if (!node.fontResourceName.empty())
        resources.insert("font:" + node.fontResourceName);
    if (resources.size() > ViewTreeLimits::MaximumResources)
    {
        error = "view tree resource limit exceeded (64)";
        return false;
    }
    if (IsLeafNode(node.type) && !node.children.empty())
    {
        error = std::string(ViewNodeTypeName(node.type)) +
            " nodes cannot have children";
        return false;
    }
    if (node.type == ViewNodeType::Button && node.text.empty())
    {
        error = "button nodes require label text";
        return false;
    }
    if (node.type == ViewNodeType::Badge && node.text.empty())
    {
        error = "badge nodes require text";
        return false;
    }
    if (IsCheckControlNode(node.type) && node.text.empty())
    {
        error = std::string(ViewNodeTypeName(node.type)) +
            " nodes require label text";
        return false;
    }
    if (IsIconNode(node.type) && node.text.empty())
    {
        error = std::string(ViewNodeTypeName(node.type)) +
            " nodes require a glyph";
        return false;
    }
    if (node.type == ViewNodeType::IconButton &&
        node.accessibilityLabel.empty())
    {
        error = "iconButton nodes require accessibility.label";
        return false;
    }
    if (node.type == ViewNodeType::Meter &&
        node.accessibilityLabel.empty())
    {
        error = "meter nodes require accessibility.label";
        return false;
    }
    for (const auto& [eventName, action] : node.events)
    {
        if (eventName != "click" && eventName != "doubleClick" &&
            eventName != "contextMenu" && eventName != "pointerEnter" &&
            eventName != "pointerLeave" && eventName != "pointerDown" &&
            eventName != "pointerUp" && eventName != "change")
        {
            error = "unsupported view event: " + eventName;
            return false;
        }
        if (action.id.empty() || action.id.size() > 128)
        {
            error = "view action id must contain 1 to 128 bytes";
            return false;
        }
    }
    if (IsCheckControlNode(node.type))
    {
        if (!node.events.contains("change") ||
            node.events.contains("click"))
        {
            error = std::string(ViewNodeTypeName(node.type)) +
                " nodes require change and reject click";
            return false;
        }
    }
    else if (node.events.contains("change"))
    {
        error = "change is reserved for controlled view nodes";
        return false;
    }
    for (const auto& child : node.children)
        if (!ValidateNode(child, depth + 1, nodes, textBytes,
                seriesPoints, keys, resources, error)) return false;
    return true;
}

bool CollectRegions(const ViewNode& node,
    std::vector<InteractionRegion>& regions, std::string& error)
{
    if (!node.visible) return true;
    if (!node.events.empty() || IsButtonNode(node.type))
    {
        if (node.frame.width <= 0.0f || node.frame.height <= 0.0f)
        {
            error = "interactive view node has an empty layout: " + node.key;
            return false;
        }
        InteractionRegion region;
        region.key = node.key;
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
                node.events.contains("click"))
            ? "hand" : node.cursor;
        region.events = node.events;
        if (node.type == ViewNodeType::Toggle)
            region.controlKind = InteractionControlKind::Toggle;
        else if (node.type == ViewNodeType::Checkbox)
            region.controlKind = InteractionControlKind::Checkbox;
        region.checked = node.checked;
        region.accessibilityRole = node.accessibilityRole.empty()
            ? (IsButtonNode(node.type) ? "button" :
                (node.type == ViewNodeType::Toggle ? "switch" :
                    (node.type == ViewNodeType::Checkbox ? "checkbox" :
                (IsDataSeriesNode(node.type) ? "img" :
                    (node.type == ViewNodeType::Meter ? "meter" :
                        (node.type == ViewNodeType::Divider
                            ? "separator" :
                            (node.type == ViewNodeType::Badge
                                ? "status" : "")))))))
            : node.accessibilityRole;
        region.accessibilityLabel = node.accessibilityLabel.empty()
            ? node.text : node.accessibilityLabel;
        region.enabled = node.enabled;
        regions.push_back(std::move(region));
    }
    for (const auto& child : node.children)
        if (!CollectRegions(child, regions, error)) return false;
    return true;
}
}

bool ValidateAndLayoutViewTree(ViewNode& root, float width, float height,
    std::string& error)
{
    error.clear();
    if (!FiniteInRange(width, 1.0f, MaximumDimension) ||
        !FiniteInRange(height, 1.0f, MaximumDimension))
    {
        error = "view surface dimensions must be finite and positive";
        return false;
    }
    std::size_t nodes = 0;
    std::size_t textBytes = 0;
    std::size_t seriesPoints = 0;
    std::unordered_set<std::string> keys;
    std::unordered_set<std::string> resources;
    if (!ValidateNode(root, 0, nodes, textBytes, seriesPoints,
            keys, resources, error))
        return false;
    LayoutNode(root, { 0.0f, 0.0f, width, height });
    return true;
}

bool CollectViewInteractionRegions(const ViewNode& root,
    std::vector<InteractionRegion>& regions, std::string& error)
{
    error.clear();
    regions.clear();
    if (!CollectRegions(root, regions, error)) return false;
    if (regions.size() > WidgetInteractionRegions::kMaximumRegions)
    {
        error = "view interaction region limit exceeded (256)";
        regions.clear();
        return false;
    }
    return true;
}

const char* ViewNodeTypeName(ViewNodeType type) noexcept
{
    switch (type)
    {
    case ViewNodeType::Box: return "box";
    case ViewNodeType::Row: return "row";
    case ViewNodeType::Column: return "column";
    case ViewNodeType::Stack: return "stack";
    case ViewNodeType::Text: return "text";
    case ViewNodeType::Image: return "image";
    case ViewNodeType::Button: return "button";
    case ViewNodeType::Toggle: return "toggle";
    case ViewNodeType::Checkbox: return "checkbox";
    case ViewNodeType::Icon: return "icon";
    case ViewNodeType::IconButton: return "iconButton";
    case ViewNodeType::Shape: return "shape";
    case ViewNodeType::Badge: return "badge";
    case ViewNodeType::Divider: return "divider";
    case ViewNodeType::ProgressBar: return "progressBar";
    case ViewNodeType::ProgressRing: return "progressRing";
    case ViewNodeType::Meter: return "meter";
    case ViewNodeType::Sparkline: return "sparkline";
    case ViewNodeType::LineChart: return "lineChart";
    case ViewNodeType::BarChart: return "barChart";
    case ViewNodeType::Waveform: return "waveform";
    case ViewNodeType::Spectrum: return "spectrum";
    case ViewNodeType::Spacer: return "spacer";
    }
    return "unknown";
}
}
