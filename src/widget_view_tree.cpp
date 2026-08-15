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

bool IsControlledNode(ViewNodeType type) noexcept
{
    return IsCheckControlNode(type) ||
        type == ViewNodeType::RadioGroup ||
        type == ViewNodeType::Slider;
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
        IsButtonNode(type) || type == ViewNodeType::Link ||
        IsControlledNode(type) ||
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
        node.type == ViewNodeType::Button ||
        node.type == ViewNodeType::Link)
        return TextIntrinsicWidth(node);
    if (node.type == ViewNodeType::Toggle)
        return TextIntrinsicWidth(node) + 44.0f;
    if (node.type == ViewNodeType::Checkbox)
        return TextIntrinsicWidth(node) + 26.0f;
    if (node.type == ViewNodeType::RadioGroup)
    {
        float result = 0.0f;
        if (node.orientation == ViewOrientation::Horizontal)
        {
            for (const auto& option : node.options)
                result += std::max(node.fontSize,
                    static_cast<float>(std::min<std::size_t>(
                        option.label.size(), 256)) * node.fontSize * 0.55f) +
                    26.0f;
            if (node.options.size() > 1)
                result += node.gap *
                    static_cast<float>(node.options.size() - 1);
        }
        else
        {
            for (const auto& option : node.options)
                result = std::max(result, std::max(node.fontSize,
                    static_cast<float>(std::min<std::size_t>(
                        option.label.size(), 256)) * node.fontSize * 0.55f) +
                    26.0f);
        }
        return result + node.padding * 2.0f;
    }
    if (node.type == ViewNodeType::Slider)
        return (node.orientation == ViewOrientation::Horizontal
            ? 96.0f : 24.0f) + node.padding * 2.0f;
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
    if (node.type == ViewNodeType::Grid)
    {
        std::vector<float> widths(node.columns, 0.0f);
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            widths[visible % node.columns] = std::max(
                widths[visible % node.columns], IntrinsicWidth(child));
            ++visible;
        }
        const std::size_t usedColumns = std::min(node.columns, visible);
        float result = 0.0f;
        for (std::size_t column = 0; column < usedColumns; ++column)
            result += widths[column];
        if (usedColumns > 1)
            result += node.columnGap.value_or(node.gap) *
                static_cast<float>(usedColumns - 1);
        return result + node.padding * 2.0f;
    }
    if (node.type == ViewNodeType::Flow)
    {
        float result = 0.0f;
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += IntrinsicWidth(child);
            ++visible;
        }
        if (visible > 1)
            result += node.columnGap.value_or(node.gap) *
                static_cast<float>(visible - 1);
        return result + node.padding * 2.0f;
    }
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
    if (node.type == ViewNodeType::Text ||
        node.type == ViewNodeType::Link)
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
    if (node.type == ViewNodeType::RadioGroup)
    {
        const float optionHeight = std::max(32.0f, node.fontSize * 1.8f);
        const float content = node.orientation == ViewOrientation::Vertical
            ? optionHeight * static_cast<float>(node.options.size()) +
                node.gap * static_cast<float>(
                    node.options.empty() ? 0 : node.options.size() - 1)
            : optionHeight;
        return content + node.padding * 2.0f;
    }
    if (node.type == ViewNodeType::Slider)
        return (node.orientation == ViewOrientation::Horizontal
            ? 24.0f : 96.0f) + node.padding * 2.0f;
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
    if (node.type == ViewNodeType::Grid)
    {
        std::vector<float> heights;
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            const std::size_t row = visible / node.columns;
            if (row >= heights.size()) heights.push_back(0.0f);
            heights[row] = std::max(heights[row], IntrinsicHeight(child));
            ++visible;
        }
        float result = 0.0f;
        for (float height : heights) result += height;
        if (heights.size() > 1)
            result += node.rowGap.value_or(node.gap) *
                static_cast<float>(heights.size() - 1);
        return result + node.padding * 2.0f;
    }
    if (node.type == ViewNodeType::Flow)
    {
        float result = 0.0f;
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += IntrinsicHeight(child);
            ++visible;
        }
        if (visible > 1)
            result += node.rowGap.value_or(node.gap) *
                static_cast<float>(visible - 1);
        return result + node.padding * 2.0f;
    }
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

void LayoutGrid(ViewNode& node, const ViewRect& content)
{
    std::vector<ViewNode*> visible;
    visible.reserve(node.children.size());
    for (auto& child : node.children)
        if (child.visible) visible.push_back(&child);
    if (visible.empty()) return;

    const std::size_t columns = std::min(node.columns, visible.size());
    const std::size_t rows =
        (visible.size() + columns - 1) / columns;
    const float columnGap = node.columnGap.value_or(node.gap);
    float rowGap = node.rowGap.value_or(node.gap);
    const float cellWidth = std::max(0.0f,
        (content.width - columnGap * static_cast<float>(columns - 1)) /
            static_cast<float>(columns));
    std::vector<float> rowHeights(rows, 0.0f);
    for (std::size_t index = 0; index < visible.size(); ++index)
        rowHeights[index / columns] = std::max(
            rowHeights[index / columns], IntrinsicHeight(*visible[index]));

    float rowsHeight = 0.0f;
    for (float height : rowHeights) rowsHeight += height;
    const float gapsHeight = rowGap * static_cast<float>(rows - 1);
    const float availableRowsHeight = std::max(
        0.0f, content.height - gapsHeight);
    if (rowsHeight > availableRowsHeight && rowsHeight > 0.0f)
    {
        const float scale = availableRowsHeight / rowsHeight;
        for (float& height : rowHeights) height *= scale;
        rowsHeight = availableRowsHeight;
    }

    float y = content.y;
    const float usedHeight = rowsHeight + gapsHeight;
    if (node.justifyContent == ViewJustification::Center)
        y += std::max(0.0f, (content.height - usedHeight) * 0.5f);
    else if (node.justifyContent == ViewJustification::End)
        y += std::max(0.0f, content.height - usedHeight);
    else if (node.justifyContent == ViewJustification::SpaceBetween &&
        rows > 1 && content.height > usedHeight)
    {
        rowGap += (content.height - usedHeight) /
            static_cast<float>(rows - 1);
    }

    for (std::size_t index = 0; index < visible.size(); ++index)
    {
        ViewNode& child = *visible[index];
        const std::size_t column = index % columns;
        const std::size_t row = index / columns;
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? node.alignItems : child.alignSelf;
        const float width = ResolveCrossSize(child.width,
            IntrinsicWidth(child), cellWidth, alignment);
        const float height = ResolveCrossSize(child.height,
            IntrinsicHeight(child), rowHeights[row], alignment);
        const float x = content.x +
            static_cast<float>(column) * (cellWidth + columnGap) +
            AlignOffset(alignment, cellWidth, width);
        LayoutNode(child, { x,
            y + AlignOffset(alignment, rowHeights[row], height),
            width, height });
        if (column + 1 == columns || index + 1 == visible.size())
            y += rowHeights[row] + rowGap;
    }
}

void LayoutFlow(ViewNode& node, const ViewRect& content)
{
    struct Item
    {
        ViewNode* node = nullptr;
        float width = 0.0f;
        float intrinsicHeight = 0.0f;
    };
    struct Line
    {
        std::vector<Item> items;
        float width = 0.0f;
        float height = 0.0f;
    };

    std::vector<Line> lines;
    const float columnGap = node.columnGap.value_or(node.gap);
    for (auto& child : node.children)
    {
        if (!child.visible) continue;
        float width = IntrinsicWidth(child);
        if (child.width.kind == ViewLengthKind::Fixed)
            width = child.width.value;
        else if (child.width.kind == ViewLengthKind::Fill)
            width = content.width;
        width = std::clamp(width, 0.0f, content.width);
        const float height = IntrinsicHeight(child);
        if (lines.empty() || (!lines.back().items.empty() &&
                lines.back().width + columnGap + width > content.width))
            lines.push_back({});
        Line& line = lines.back();
        if (!line.items.empty()) line.width += columnGap;
        line.width += width;
        line.height = std::max(line.height, height);
        line.items.push_back({ &child, width, height });
    }
    if (lines.empty()) return;

    float rowGap = node.rowGap.value_or(node.gap);
    float linesHeight = 0.0f;
    for (const auto& line : lines) linesHeight += line.height;
    const float gapHeight = rowGap * static_cast<float>(lines.size() - 1);
    const float availableLinesHeight = std::max(
        0.0f, content.height - gapHeight);
    if (linesHeight > availableLinesHeight && linesHeight > 0.0f)
    {
        const float scale = availableLinesHeight / linesHeight;
        for (auto& line : lines) line.height *= scale;
        linesHeight = availableLinesHeight;
    }

    float y = content.y;
    for (auto& line : lines)
    {
        float x = content.x;
        float dynamicGap = columnGap;
        if (node.justifyContent == ViewJustification::Center)
            x += std::max(0.0f, (content.width - line.width) * 0.5f);
        else if (node.justifyContent == ViewJustification::End)
            x += std::max(0.0f, content.width - line.width);
        else if (node.justifyContent == ViewJustification::SpaceBetween &&
            line.items.size() > 1 && content.width > line.width)
        {
            dynamicGap += (content.width - line.width) /
                static_cast<float>(line.items.size() - 1);
        }
        for (const Item& item : line.items)
        {
            ViewNode& child = *item.node;
            const ViewAlignment alignment =
                child.alignSelf == ViewAlignment::Auto
                ? node.alignItems : child.alignSelf;
            const float height = ResolveCrossSize(child.height,
                item.intrinsicHeight, line.height, alignment);
            LayoutNode(child, { x,
                y + AlignOffset(alignment, line.height, height),
                item.width, height });
            x += item.width + dynamicGap;
        }
        y += line.height + rowGap;
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
    else if (node.type == ViewNodeType::Grid)
        LayoutGrid(node, content);
    else if (node.type == ViewNodeType::Flow)
        LayoutFlow(node, content);
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
        (node.columnGap && !FiniteInRange(*node.columnGap, 0.0f, 4096.0f)) ||
        (node.rowGap && !FiniteInRange(*node.rowGap, 0.0f, 4096.0f)) ||
        !FiniteInRange(node.flexGrow, 0.0f, 1000.0f) ||
        !FiniteInRange(node.fontSize, 1.0f, 512.0f) ||
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
    if ((node.type == ViewNodeType::ProgressBar ||
            node.type == ViewNodeType::ProgressRing ||
            node.type == ViewNodeType::Meter) &&
        !FiniteInRange(node.value, 0.0f, 1.0f))
    {
        error = "progress values must be finite and between 0 and 1";
        return false;
    }
    if (node.type == ViewNodeType::Slider &&
        (!FiniteInRange(node.minimum, -1.0e9f, 1.0e9f) ||
            !FiniteInRange(node.maximum, -1.0e9f, 1.0e9f) ||
            !FiniteInRange(node.value, -1.0e9f, 1.0e9f) ||
            !FiniteInRange(node.step, 0.000001f, 1.0e9f) ||
            node.minimum >= node.maximum || node.value < node.minimum ||
            node.value > node.maximum ||
            node.step > node.maximum - node.minimum))
    {
        error = "slider requires min < max, a value in range, and a positive bounded step";
        return false;
    }
    if (node.type == ViewNodeType::Grid &&
        (node.columns == 0 || node.columns > 64))
    {
        error = "grid columns must be between 1 and 64";
        return false;
    }
    if (node.type != ViewNodeType::Grid && node.columns != 1)
    {
        error = "grid columns are reserved for grid nodes";
        return false;
    }
    if (node.type != ViewNodeType::Grid &&
        node.type != ViewNodeType::Flow &&
        (node.columnGap || node.rowGap))
    {
        error = "columnGap and rowGap are reserved for grid and flow nodes";
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
    if (node.type == ViewNodeType::RadioGroup)
    {
        if (node.options.empty() ||
            node.options.size() > ViewTreeLimits::MaximumChoiceOptions)
        {
            error = "radioGroup options must contain 1 to 64 items";
            return false;
        }
        std::unordered_set<std::string> optionKeys;
        std::unordered_set<std::string> optionValues;
        bool selectionFound = node.selectedValue.empty();
        for (const auto& option : node.options)
        {
            const std::string regionKey = node.key + "/" + option.key;
            if (option.key.empty() || option.key.size() > 128 ||
                option.value.empty() ||
                option.value.size() > ViewTreeLimits::MaximumTextBytes ||
                option.label.empty() ||
                option.label.size() > ViewTreeLimits::MaximumTextBytes ||
                regionKey.size() > 128)
            {
                error = "radioGroup option keys, values, and labels must be non-empty and bounded";
                return false;
            }
            if (!optionKeys.insert(option.key).second ||
                !optionValues.insert(option.value).second)
            {
                error = "radioGroup option keys and values must be unique";
                return false;
            }
            if (!keys.insert(regionKey).second)
            {
                error = "duplicate generated radio option key: " + regionKey;
                return false;
            }
            const std::size_t optionBytes = option.key.size() +
                option.value.size() + option.label.size();
            if (optionBytes >
                ViewTreeLimits::MaximumTotalTextBytes - textBytes)
            {
                error = "view tree text limit exceeded";
                return false;
            }
            textBytes += optionBytes;
            if (option.value == node.selectedValue) selectionFound = true;
        }
        if (!selectionFound)
        {
            error = "radioGroup selectedValue must match an option or be empty";
            return false;
        }
    }
    else if (!node.options.empty() || !node.selectedValue.empty())
    {
        error = "radio options are reserved for radioGroup nodes";
        return false;
    }
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
        node.type != ViewNodeType::Link &&
        node.type != ViewNodeType::Badge &&
        !IsCheckControlNode(node.type) &&
        node.type != ViewNodeType::RadioGroup)
    {
        error = "only text and label-bearing nodes can retain a font resource";
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
    if (node.type == ViewNodeType::Link && node.text.empty())
    {
        error = "link nodes require label text";
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
    if (node.type == ViewNodeType::Slider &&
        node.accessibilityLabel.empty())
    {
        error = "slider nodes require accessibility.label";
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
    if (IsControlledNode(node.type))
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
    if (node.type == ViewNodeType::Link &&
        !node.events.contains("click"))
    {
        error = "link nodes require a click action";
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
    if (node.type == ViewNodeType::RadioGroup)
    {
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            if (regions.size() >=
                WidgetInteractionRegions::kMaximumRegions)
            {
                error = "view interaction region limit exceeded (256)";
                return false;
            }
            const auto& option = node.options[index];
            const ViewRect frame = ViewRadioOptionFrame(node, index);
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
            region.cursor = node.cursor.empty() ? "hand" : node.cursor;
            region.events = node.events;
            region.controlKind = InteractionControlKind::Radio;
            region.checked = option.value == node.selectedValue;
            region.currentSelection = node.selectedValue;
            region.proposedSelection = option.value;
            region.accessibilityRole = "radio";
            region.accessibilityLabel = option.label;
            region.enabled = node.enabled && option.enabled;
            regions.push_back(std::move(region));
        }
        return true;
    }
    if (!node.events.empty() || IsButtonNode(node.type))
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
                node.events.contains("click"))
            ? "hand" : node.cursor;
        region.events = node.events;
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
            const float mainLength = region.vertical
                ? node.frame.height : node.frame.width;
            const float inset = std::min(node.padding + thumbRadius,
                std::max(0.0f, mainLength * 0.5f));
            region.controlStart = (region.vertical
                ? node.frame.y : node.frame.x) + inset;
            region.controlLength = std::max(0.0f,
                mainLength - inset * 2.0f);
        }
        region.checked = node.checked;
        region.accessibilityRole = node.accessibilityRole.empty()
            ? (IsButtonNode(node.type) ? "button" :
                (node.type == ViewNodeType::Link ? "link" :
                    (node.type == ViewNodeType::Slider ? "slider" :
                (node.type == ViewNodeType::Toggle ? "switch" :
                    (node.type == ViewNodeType::Checkbox ? "checkbox" :
                (IsDataSeriesNode(node.type) ? "img" :
                    (node.type == ViewNodeType::Meter ? "meter" :
                        (node.type == ViewNodeType::Divider
                            ? "separator" :
                            (node.type == ViewNodeType::Badge
                                ? "status" : "")))))))))
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

ViewRect ViewRadioOptionFrame(
    const ViewNode& node, std::size_t optionIndex) noexcept
{
    if (node.options.empty() || optionIndex >= node.options.size())
        return {};
    const float inset = std::min(node.padding,
        std::max(0.0f, std::min(node.frame.width, node.frame.height) * 0.5f));
    const ViewRect content{
        node.frame.x + inset,
        node.frame.y + inset,
        std::max(0.0f, node.frame.width - inset * 2.0f),
        std::max(0.0f, node.frame.height - inset * 2.0f),
    };
    const float count = static_cast<float>(node.options.size());
    if (node.orientation == ViewOrientation::Horizontal)
    {
        const float width = std::max(0.0f,
            (content.width - node.gap * (count - 1.0f)) / count);
        return { content.x + static_cast<float>(optionIndex) *
                (width + node.gap), content.y, width, content.height };
    }
    const float height = std::max(0.0f,
        (content.height - node.gap * (count - 1.0f)) / count);
    return { content.x, content.y + static_cast<float>(optionIndex) *
            (height + node.gap), content.width, height };
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
    case ViewNodeType::Grid: return "grid";
    case ViewNodeType::Flow: return "flow";
    case ViewNodeType::Stack: return "stack";
    case ViewNodeType::Text: return "text";
    case ViewNodeType::Image: return "image";
    case ViewNodeType::Button: return "button";
    case ViewNodeType::Link: return "link";
    case ViewNodeType::Toggle: return "toggle";
    case ViewNodeType::Checkbox: return "checkbox";
    case ViewNodeType::RadioGroup: return "radioGroup";
    case ViewNodeType::Slider: return "slider";
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
