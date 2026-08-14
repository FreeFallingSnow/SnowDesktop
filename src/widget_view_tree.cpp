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

float IntrinsicWidth(const ViewNode& node);
float IntrinsicHeight(const ViewNode& node);

float IntrinsicWidth(const ViewNode& node)
{
    if (node.width.kind == ViewLengthKind::Fixed)
        return node.width.value;
    if (node.type == ViewNodeType::Text ||
        node.type == ViewNodeType::Button)
        return TextIntrinsicWidth(node);
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
    if (node.type == ViewNodeType::Button)
        return std::max(32.0f, node.fontSize * 1.8f) +
            node.padding * 2.0f;
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
    std::unordered_set<std::string>& keys, std::string& error)
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
        !validStyle(node.style) || !validStyle(node.hoverStyle) ||
        !validStyle(node.pressedStyle))
    {
        error = "view node dimensions and typography must be finite and bounded";
        return false;
    }
    if (node.text.size() > ViewTreeLimits::MaximumTextBytes ||
        textBytes + node.text.size() >
            ViewTreeLimits::MaximumTotalTextBytes)
    {
        error = "view tree text limit exceeded";
        return false;
    }
    textBytes += node.text.size();
    if ((node.type == ViewNodeType::Text ||
            node.type == ViewNodeType::Button ||
            node.type == ViewNodeType::Spacer) &&
        !node.children.empty())
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
    for (const auto& [eventName, action] : node.events)
    {
        if (eventName != "click" && eventName != "doubleClick" &&
            eventName != "contextMenu" && eventName != "pointerEnter" &&
            eventName != "pointerLeave" && eventName != "pointerDown" &&
            eventName != "pointerUp")
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
    for (const auto& child : node.children)
        if (!ValidateNode(child, depth + 1, nodes, textBytes,
                keys, error)) return false;
    return true;
}

bool CollectRegions(const ViewNode& node,
    std::vector<InteractionRegion>& regions, std::string& error)
{
    if (!node.visible) return true;
    if (!node.events.empty() || node.type == ViewNodeType::Button)
    {
        if (node.frame.width <= 0.0f || node.frame.height <= 0.0f)
        {
            error = "interactive view node has an empty layout: " + node.key;
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
        region.cursor = node.cursor.empty() &&
            (node.type == ViewNodeType::Button ||
                node.events.contains("click"))
            ? "hand" : node.cursor;
        region.events = node.events;
        region.accessibilityRole = node.accessibilityRole.empty() &&
            node.type == ViewNodeType::Button
            ? "button" : node.accessibilityRole;
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
    std::unordered_set<std::string> keys;
    if (!ValidateNode(root, 0, nodes, textBytes, keys, error))
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
    case ViewNodeType::Button: return "button";
    case ViewNodeType::Spacer: return "spacer";
    }
    return "unknown";
}
}
