#include "widget_view_layout.h"
#include "widget_view_node_rules.h"
#include "widget_view_tree.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
namespace detail
{
namespace
{
struct ResolvedNodeSize
{
    float width = 0.0f;
    float height = 0.0f;
};

ResolvedNodeSize ResolveNodeSize(
    const ViewNode& node, float proposedWidth, float proposedHeight) noexcept
{
    ResolvedNodeSize result{
        ConstrainWidth(node, proposedWidth),
        ConstrainHeight(node, proposedHeight),
    };
    if (!node.aspectRatio) return result;

    const float ratio = *node.aspectRatio;
    const float minimumWidth = std::max(
        node.minimumWidth.value_or(0.0f),
        node.minimumHeight.value_or(0.0f) * ratio);
    const float maximumWidth = std::min(
        node.maximumWidth.value_or(MaximumDimension),
        node.maximumHeight.value_or(MaximumDimension) * ratio);

    float width = 0.0f;
    if (node.width.kind == ViewLengthKind::Fixed &&
        node.height.kind != ViewLengthKind::Fixed)
        width = result.width;
    else if (node.height.kind == ViewLengthKind::Fixed &&
        node.width.kind != ViewLengthKind::Fixed)
        width = result.height * ratio;
    else if (node.width.kind == ViewLengthKind::Fill &&
        node.height.kind == ViewLengthKind::Auto)
        width = result.width;
    else if (node.height.kind == ViewLengthKind::Fill &&
        node.width.kind == ViewLengthKind::Auto)
        width = result.height * ratio;
    else
        width = std::min(result.width, result.height * ratio);

    width = std::clamp(width, minimumWidth,
        std::max(minimumWidth, maximumWidth));
    result.width = width;
    result.height = width / ratio;
    return result;
}

float HorizontalInsets(const ViewEdgeInsets& insets) noexcept
{
    return insets.left + insets.right;
}

float VerticalInsets(const ViewEdgeInsets& insets) noexcept
{
    return insets.top + insets.bottom;
}

ViewEdgeInsets ResolveInsets(const ViewEdgeInsets& insets,
    float width, float height) noexcept
{
    ViewEdgeInsets resolved = insets;
    const float horizontal = HorizontalInsets(resolved);
    if (horizontal > width && horizontal > 0.0f)
    {
        const float scale = std::max(0.0f, width) / horizontal;
        resolved.left *= scale;
        resolved.right *= scale;
    }
    const float vertical = VerticalInsets(resolved);
    if (vertical > height && vertical > 0.0f)
    {
        const float scale = std::max(0.0f, height) / vertical;
        resolved.top *= scale;
        resolved.bottom *= scale;
    }
    return resolved;
}

float ViewHorizontalMargin(const ViewNode& node) noexcept
{
    return HorizontalInsets(node.margin);
}

float ViewVerticalMargin(const ViewNode& node) noexcept
{
    return VerticalInsets(node.margin);
}

float ViewHorizontalPadding(const ViewNode& node) noexcept
{
    return HorizontalInsets(node.padding);
}

float ViewVerticalPadding(const ViewNode& node) noexcept
{
    return VerticalInsets(node.padding);
}

ResolvedNodeSize ResolveOuterNodeSize(
    const ViewNode& node, float proposedWidth, float proposedHeight) noexcept
{
    const float horizontalMargin = ViewHorizontalMargin(node);
    const float verticalMargin = ViewVerticalMargin(node);
    const ResolvedNodeSize inner = ResolveNodeSize(node,
        std::max(0.0f, proposedWidth - horizontalMargin),
        std::max(0.0f, proposedHeight - verticalMargin));
    return { inner.width + horizontalMargin,
        inner.height + verticalMargin };
}

float TextIntrinsicWidth(const ViewNode& node) noexcept
{
    const float approximateGlyphs = static_cast<float>(
        std::min<std::size_t>(node.text.size(), 256));
    const float spacing = std::max(0.0f, approximateGlyphs - 1.0f) *
        node.letterSpacing;
    return std::max(node.fontSize,
        approximateGlyphs * node.fontSize * 0.55f + spacing) +
        ViewHorizontalPadding(node);
}

float StyledTextIntrinsicWidth(const ViewNode& node) noexcept
{
    float width = 0.0f;
    float approximateGlyphs = 0.0f;
    for (const auto& span : node.spans)
    {
        const float size = span.fontSize.value_or(node.fontSize);
        const float glyphs = span.icon ? 1.0f : static_cast<float>(
            std::min<std::size_t>(span.text.size(), 256));
        width += span.icon ? size : glyphs * size * 0.55f;
        approximateGlyphs += glyphs;
    }
    width += std::max(0.0f, approximateGlyphs - 1.0f) *
        node.letterSpacing;
    return std::max(node.fontSize, width) + ViewHorizontalPadding(node);
}

float TextIntrinsicLineHeight(const ViewNode& node) noexcept
{
    if (node.lineHeight) return *node.lineHeight;
    float fontSize = node.fontSize;
    if (node.type == ViewNodeType::StyledText)
    {
        for (const auto& span : node.spans)
            fontSize = std::max(fontSize,
                span.fontSize.value_or(node.fontSize));
    }
    return fontSize * 1.4f;
}

bool IsHorizontalFlex(const ViewNode& node) noexcept
{
    if (node.flexDirection == ViewFlexDirection::Row ||
        node.flexDirection == ViewFlexDirection::RowReverse) return true;
    if (node.flexDirection == ViewFlexDirection::Column ||
        node.flexDirection == ViewFlexDirection::ColumnReverse) return false;
    return node.type == ViewNodeType::Row;
}

bool IsFlexMainReversed(const ViewNode& node) noexcept
{
    return node.flexDirection == ViewFlexDirection::RowReverse ||
        node.flexDirection == ViewFlexDirection::ColumnReverse;
}

ViewRect ContentRect(const ViewNode& node) noexcept
{
    const ViewEdgeInsets insets = ResolveInsets(
        node.padding, node.frame.width, node.frame.height);
    return { node.frame.x + insets.left, node.frame.y + insets.top,
        std::max(0.0f, node.frame.width - HorizontalInsets(insets)),
        std::max(0.0f, node.frame.height - VerticalInsets(insets)) };
}

float IntrinsicWidth(const ViewNode& node);
float IntrinsicHeight(const ViewNode& node);
float RawIntrinsicWidth(const ViewNode& node);
float RawIntrinsicHeight(const ViewNode& node);

float OuterIntrinsicWidth(const ViewNode& node)
{
    return IntrinsicWidth(node) + ViewHorizontalMargin(node);
}

float OuterIntrinsicHeight(const ViewNode& node)
{
    return IntrinsicHeight(node) + ViewVerticalMargin(node);
}

const ViewGridTrack& GridTrackAt(
    const std::vector<ViewGridTrack>& tracks, std::size_t index) noexcept
{
    static constexpr ViewGridTrack implicitAuto{};
    return index < tracks.size() ? tracks[index] : implicitAuto;
}

float GridTrackBaseSize(const ViewGridTrack& track) noexcept
{
    if (track.kind == ViewGridTrackKind::Fixed) return track.value;
    if (track.kind == ViewGridTrackKind::MinMax) return track.minimum;
    return 0.0f;
}

float GridTrackGrowthCapacity(const ViewGridTrack& track,
    float current) noexcept
{
    if (track.kind == ViewGridTrackKind::Fixed) return 0.0f;
    if (track.kind == ViewGridTrackKind::MinMax &&
        track.maximumKind == ViewGridTrackKind::Fixed)
        return std::max(0.0f, track.maximumValue - current);
    return std::numeric_limits<float>::infinity();
}

void GrowGridTrackRange(const std::vector<ViewGridTrack>& tracks,
    std::vector<float>& sizes, std::size_t start, std::size_t span,
    float amount)
{
    std::vector<std::size_t> active;
    active.reserve(span);
    for (std::size_t index = start; index < start + span; ++index)
        if (GridTrackGrowthCapacity(GridTrackAt(tracks, index),
                sizes[index]) > 0.001f)
            active.push_back(index);
    while (amount > 0.001f && !active.empty())
    {
        const float share = amount / static_cast<float>(active.size());
        float applied = 0.0f;
        std::vector<std::size_t> remaining;
        remaining.reserve(active.size());
        for (const std::size_t index : active)
        {
            const float capacity = GridTrackGrowthCapacity(
                GridTrackAt(tracks, index), sizes[index]);
            const float delta = std::min(share, capacity);
            sizes[index] += delta;
            applied += delta;
            if (capacity > delta + 0.001f) remaining.push_back(index);
        }
        if (applied <= 0.001f) break;
        amount -= applied;
        active = std::move(remaining);
    }
}

std::vector<float> ResolveGridTrackSizes(const ViewNode& node,
    const std::vector<ViewGridTrack>& tracks, std::size_t count,
    bool horizontal, float gap, std::optional<float> available)
{
    std::vector<float> sizes(count, 0.0f);
    for (std::size_t index = 0; index < count; ++index)
        sizes[index] = GridTrackBaseSize(GridTrackAt(tracks, index));

    for (const ViewNode& child : node.children)
    {
        if (!child.visible) continue;
        const std::size_t start = horizontal
            ? child.resolvedGridColumn : child.resolvedGridRow;
        const std::size_t requestedSpan = horizontal
            ? child.columnSpan : child.rowSpan;
        if (start >= count) continue;
        const std::size_t span = std::min(requestedSpan, count - start);
        float current = gap * static_cast<float>(span - 1);
        for (std::size_t index = start; index < start + span; ++index)
            current += sizes[index];
        const float intrinsic = horizontal
            ? OuterIntrinsicWidth(child) : OuterIntrinsicHeight(child);
        GrowGridTrackRange(tracks, sizes, start, span,
            std::max(0.0f, intrinsic - current));
    }

    if (!available) return sizes;
    const float gapExtent = gap * static_cast<float>(count > 0
        ? count - 1 : 0);
    float used = 0.0f;
    for (const float size : sizes) used += size;
    float extra = std::max(0.0f, *available - gapExtent - used);

    if (extra > 0.001f)
    {
        std::vector<std::size_t> capped;
        for (std::size_t index = 0; index < count; ++index)
        {
            const ViewGridTrack& track = GridTrackAt(tracks, index);
            if (track.kind == ViewGridTrackKind::MinMax &&
                track.maximumKind == ViewGridTrackKind::Fixed &&
                sizes[index] + 0.001f < track.maximumValue)
                capped.push_back(index);
        }
        while (extra > 0.001f && !capped.empty())
        {
            const float share = extra / static_cast<float>(capped.size());
            float applied = 0.0f;
            std::vector<std::size_t> remaining;
            for (const std::size_t index : capped)
            {
                const float capacity = std::max(
                    0.0f, GridTrackAt(tracks, index).maximumValue -
                        sizes[index]);
                const float delta = std::min(share, capacity);
                sizes[index] += delta;
                applied += delta;
                if (capacity > delta + 0.001f) remaining.push_back(index);
            }
            if (applied <= 0.001f) break;
            extra -= applied;
            capped = std::move(remaining);
        }
    }

    float fractionTotal = 0.0f;
    for (std::size_t index = 0; index < count; ++index)
    {
        const ViewGridTrack& track = GridTrackAt(tracks, index);
        if (track.kind == ViewGridTrackKind::Fraction)
            fractionTotal += track.value;
        else if (track.kind == ViewGridTrackKind::MinMax &&
            track.maximumKind == ViewGridTrackKind::Fraction)
            fractionTotal += track.maximumValue;
    }
    if (extra > 0.001f && fractionTotal > 0.0f)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            const ViewGridTrack& track = GridTrackAt(tracks, index);
            float weight = 0.0f;
            if (track.kind == ViewGridTrackKind::Fraction)
                weight = track.value;
            else if (track.kind == ViewGridTrackKind::MinMax &&
                track.maximumKind == ViewGridTrackKind::Fraction)
                weight = track.maximumValue;
            sizes[index] += extra * (weight / fractionTotal);
        }
    }
    return sizes;
}

float IntrinsicWidth(const ViewNode& node)
{
    return ConstrainWidth(node, RawIntrinsicWidth(node));
}

float RawIntrinsicWidth(const ViewNode& node)
{
    if (node.width.kind == ViewLengthKind::Fixed)
        return node.width.value;
    if (node.type == ViewNodeType::StyledText)
        return StyledTextIntrinsicWidth(node);
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
        return result + ViewHorizontalPadding(node);
    }
    if (IsInputNode(node.type) || node.type == ViewNodeType::Select)
        return 144.0f + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::Slider)
        return (node.orientation == ViewOrientation::Horizontal
            ? 96.0f : 24.0f) + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::Image ||
        node.type == ViewNodeType::ReferenceIcon)
        return 48.0f + ViewHorizontalPadding(node);
    if (IsIconNode(node.type))
        return node.fontSize * 1.4f + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::ProgressRing)
        return 32.0f + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::Shape)
        return 8.0f + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::Divider)
        return (node.orientation == ViewOrientation::Vertical
            ? node.thickness : 24.0f) + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::Meter)
        return 64.0f + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::MonthCalendar)
        return 224.0f + ViewHorizontalPadding(node);
    if (IsDataSeriesNode(node.type))
        return 64.0f + ViewHorizontalPadding(node);
    if (node.type == ViewNodeType::Spacer)
        return 0.0f;
    if (HasCollectionPlaceholder(node))
    {
        float result = 0.0f;
        for (const auto& child : node.children)
            if (child.visible)
                result = std::max(result, OuterIntrinsicWidth(child));
        return result + ViewHorizontalPadding(node);
    }
    if (IsVirtualCollection(node.type))
    {
        if (node.type == ViewNodeType::VirtualList &&
            node.orientation == ViewOrientation::Horizontal)
        {
            const double extent =
                static_cast<double>(node.itemCount) * node.itemExtent +
                static_cast<double>(node.itemCount > 0
                    ? node.itemCount - 1 : 0) *
                    VirtualCollectionMainGap(node);
            return static_cast<float>(std::min<double>(
                MaximumScrollExtent, extent)) + ViewHorizontalPadding(node);
        }
        float cellWidth = 0.0f;
        for (const auto& child : node.children)
            if (child.visible)
                cellWidth = std::max(cellWidth, OuterIntrinsicWidth(child));
        const std::size_t columns = node.type == ViewNodeType::VirtualGrid
            ? node.columns : 1;
        return cellWidth * static_cast<float>(columns) +
            node.columnGap.value_or(node.gap) *
                static_cast<float>(columns > 0 ? columns - 1 : 0) +
            ViewHorizontalPadding(node);
    }
    if (IsGridContainer(node.type))
    {
        if (!node.columnTracks.empty())
        {
            const float columnGap = node.columnGap.value_or(node.gap);
            const auto widths = ResolveGridTrackSizes(node,
                node.columnTracks, node.columns, true, columnGap,
                std::nullopt);
            float result = 0.0f;
            for (const float width : widths) result += width;
            if (widths.size() > 1)
                result += columnGap *
                    static_cast<float>(widths.size() - 1);
            return result + ViewHorizontalPadding(node);
        }
        std::vector<float> widths(node.columns, 0.0f);
        std::size_t usedColumns = 0;
        const float columnGap = node.columnGap.value_or(node.gap);
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            const std::size_t column = child.resolvedGridColumn;
            const std::size_t span = std::min(
                child.columnSpan, node.columns - column);
            const float contribution = std::max(0.0f,
                (OuterIntrinsicWidth(child) -
                    columnGap * static_cast<float>(span - 1)) /
                    static_cast<float>(span));
            for (std::size_t track = column;
                track < column + span; ++track)
                widths[track] = std::max(widths[track], contribution);
            usedColumns = std::max(usedColumns, column + span);
        }
        float result = 0.0f;
        for (std::size_t column = 0; column < usedColumns; ++column)
            result += widths[column];
        if (usedColumns > 1)
            result += columnGap *
                static_cast<float>(usedColumns - 1);
        return result + ViewHorizontalPadding(node);
    }
    if (node.type == ViewNodeType::Flow)
    {
        float result = 0.0f;
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += OuterIntrinsicWidth(child);
            ++visible;
        }
        if (visible > 1)
            result += node.columnGap.value_or(node.gap) *
                static_cast<float>(visible - 1);
        return result + ViewHorizontalPadding(node);
    }
    float result = 0.0f;
    if ((IsFlexContainer(node.type) && IsHorizontalFlex(node)) ||
        (node.type == ViewNodeType::List &&
            node.orientation == ViewOrientation::Horizontal))
    {
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += OuterIntrinsicWidth(child);
            ++visible;
        }
        if (visible > 1)
            result += node.gap * static_cast<float>(visible - 1);
    }
    else if (IsFlexContainer(node.type) &&
        node.flexWrap != ViewFlexWrap::NoWrap)
    {
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += OuterIntrinsicWidth(child);
            ++visible;
        }
        if (visible > 1)
            result += node.gap * static_cast<float>(visible - 1);
    }
    else
    {
        for (const auto& child : node.children)
            if (child.visible)
                result = std::max(result, OuterIntrinsicWidth(child));
    }
    return result + ViewHorizontalPadding(node);
}

float IntrinsicHeight(const ViewNode& node)
{
    return ConstrainHeight(node, RawIntrinsicHeight(node));
}

float RawIntrinsicHeight(const ViewNode& node)
{
    if (node.height.kind == ViewLengthKind::Fixed)
        return node.height.value;
    if (node.type == ViewNodeType::Text ||
        node.type == ViewNodeType::StyledText ||
        node.type == ViewNodeType::Link)
        return TextIntrinsicLineHeight(node) + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Badge)
        return std::max(20.0f, TextIntrinsicLineHeight(node)) +
            ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Image ||
        node.type == ViewNodeType::ReferenceIcon)
        return 48.0f + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Button)
        return std::max(32.0f, TextIntrinsicLineHeight(node)) +
            ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Toggle ||
        node.type == ViewNodeType::Checkbox)
        return std::max(32.0f, TextIntrinsicLineHeight(node)) +
            ViewVerticalPadding(node);
    if (node.type == ViewNodeType::RadioGroup)
    {
        const float optionHeight = std::max(
            32.0f, TextIntrinsicLineHeight(node));
        const float content = node.orientation == ViewOrientation::Vertical
            ? optionHeight * static_cast<float>(node.options.size()) +
                node.gap * static_cast<float>(
                    node.options.empty() ? 0 : node.options.size() - 1)
            : optionHeight;
        return content + ViewVerticalPadding(node);
    }
    if (node.type == ViewNodeType::TextArea)
        return 96.0f + ViewVerticalPadding(node);
    if (IsInputNode(node.type) || node.type == ViewNodeType::Select)
        return std::max(36.0f, node.fontSize * 1.8f) +
            ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Slider)
        return (node.orientation == ViewOrientation::Horizontal
            ? 24.0f : 96.0f) + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::IconButton)
        return std::max(32.0f, node.fontSize * 1.4f) +
            ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Icon)
        return node.fontSize * 1.4f + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::ProgressBar ||
        node.type == ViewNodeType::Meter)
        return std::max(4.0f, node.thickness) + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::ProgressRing)
        return 32.0f + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Shape)
        return 8.0f + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Divider)
        return (node.orientation == ViewOrientation::Horizontal
            ? node.thickness : 24.0f) + ViewVerticalPadding(node);
    if (IsDataSeriesNode(node.type))
        return 40.0f + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::MonthCalendar)
        return 224.0f + ViewVerticalPadding(node);
    if (node.type == ViewNodeType::Spacer)
        return 0.0f;
    if (HasCollectionPlaceholder(node))
    {
        float result = 0.0f;
        for (const auto& child : node.children)
            if (child.visible)
                result = std::max(result, OuterIntrinsicHeight(child));
        return result + ViewVerticalPadding(node);
    }
    if (IsVirtualCollection(node.type))
    {
        if (node.type == ViewNodeType::VirtualList &&
            node.orientation == ViewOrientation::Horizontal)
        {
            float result = 0.0f;
            for (const auto& child : node.children)
                if (child.visible)
                    result = std::max(result, OuterIntrinsicHeight(child));
            return result + ViewVerticalPadding(node);
        }
        const std::size_t columns = node.type == ViewNodeType::VirtualGrid
            ? node.columns : 1;
        const std::size_t rows = columns == 0 ? 0 :
            (node.itemCount + columns - 1) / columns;
        const double extent = static_cast<double>(rows) * node.itemExtent +
            static_cast<double>(rows > 0 ? rows - 1 : 0) *
                node.rowGap.value_or(node.gap);
        return static_cast<float>(std::min<double>(
            MaximumScrollExtent, extent)) + ViewVerticalPadding(node);
    }
    if (IsGridContainer(node.type))
    {
        if (!node.rowTracks.empty())
        {
            std::size_t rows = node.rowTracks.size();
            for (const auto& child : node.children)
                if (child.visible)
                    rows = std::max(rows,
                        child.resolvedGridRow + child.rowSpan);
            const float rowGap = node.rowGap.value_or(node.gap);
            const auto heights = ResolveGridTrackSizes(node,
                node.rowTracks, rows, false, rowGap, std::nullopt);
            float result = 0.0f;
            for (const float height : heights) result += height;
            if (heights.size() > 1)
                result += rowGap *
                    static_cast<float>(heights.size() - 1);
            return result + ViewVerticalPadding(node);
        }
        std::vector<float> heights;
        const float rowGap = node.rowGap.value_or(node.gap);
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            const std::size_t requiredRows =
                child.resolvedGridRow + child.rowSpan;
            if (heights.size() < requiredRows)
                heights.resize(requiredRows, 0.0f);
            float current = rowGap *
                static_cast<float>(child.rowSpan - 1);
            for (std::size_t row = child.resolvedGridRow;
                row < requiredRows; ++row)
                current += heights[row];
            const float addition = std::max(
                0.0f, OuterIntrinsicHeight(child) - current) /
                static_cast<float>(child.rowSpan);
            for (std::size_t row = child.resolvedGridRow;
                row < requiredRows; ++row)
                heights[row] += addition;
        }
        float result = 0.0f;
        for (float height : heights) result += height;
        if (heights.size() > 1)
            result += rowGap *
                static_cast<float>(heights.size() - 1);
        return result + ViewVerticalPadding(node);
    }
    if (node.type == ViewNodeType::Flow)
    {
        float result = 0.0f;
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += OuterIntrinsicHeight(child);
            ++visible;
        }
        if (visible > 1)
            result += node.rowGap.value_or(node.gap) *
                static_cast<float>(visible - 1);
        return result + ViewVerticalPadding(node);
    }
    float result = 0.0f;
    if ((IsFlexContainer(node.type) && !IsHorizontalFlex(node)) ||
        (node.type == ViewNodeType::List &&
            node.orientation == ViewOrientation::Vertical) ||
        (IsFlexContainer(node.type) &&
            node.flexWrap != ViewFlexWrap::NoWrap))
    {
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            result += OuterIntrinsicHeight(child);
            ++visible;
        }
        if (visible > 1)
            result += node.gap * static_cast<float>(visible - 1);
    }
    else
    {
        for (const auto& child : node.children)
            if (child.visible)
                result = std::max(result, OuterIntrinsicHeight(child));
    }
    return result + ViewVerticalPadding(node);
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

float ResolveOuterCrossSize(const ViewNode& node,
    const ViewLength& length, float intrinsic, float available,
    ViewAlignment alignment, bool horizontalAxis) noexcept
{
    const float margin = horizontalAxis
        ? ViewHorizontalMargin(node) : ViewVerticalMargin(node);
    return ResolveCrossSize(length, intrinsic,
        std::max(0.0f, available - margin), alignment) + margin;
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

struct AxisSpacing
{
    float leading = 0.0f;
    float gap = 0.0f;
};

AxisSpacing ResolveAxisSpacing(ViewJustification alignment,
    float baseGap, float available, float used,
    std::size_t count) noexcept
{
    AxisSpacing result{ 0.0f, baseGap };
    const float extra = std::max(0.0f, available - used);
    if (alignment == ViewJustification::Center)
        result.leading = extra * 0.5f;
    else if (alignment == ViewJustification::End)
        result.leading = extra;
    else if (alignment == ViewJustification::SpaceBetween && count > 1)
        result.gap += extra / static_cast<float>(count - 1);
    else if (alignment == ViewJustification::SpaceAround && count > 0)
    {
        const float share = extra / static_cast<float>(count);
        result.leading = share * 0.5f;
        result.gap += share;
    }
    else if (alignment == ViewJustification::SpaceEvenly && count > 0)
    {
        const float share = extra / static_cast<float>(count + 1);
        result.leading = share;
        result.gap += share;
    }
    return result;
}

AxisSpacing ResolveAxisSpacing(ViewContentAlignment alignment,
    float baseGap, float available, float used,
    std::size_t count) noexcept
{
    AxisSpacing result{ 0.0f, baseGap };
    const float extra = std::max(0.0f, available - used);
    if (alignment == ViewContentAlignment::Center)
        result.leading = extra * 0.5f;
    else if (alignment == ViewContentAlignment::End)
        result.leading = extra;
    else if (alignment == ViewContentAlignment::SpaceBetween && count > 1)
        result.gap += extra / static_cast<float>(count - 1);
    else if (alignment == ViewContentAlignment::SpaceAround && count > 0)
    {
        const float share = extra / static_cast<float>(count);
        result.leading = share * 0.5f;
        result.gap += share;
    }
    else if (alignment == ViewContentAlignment::SpaceEvenly && count > 0)
    {
        const float share = extra / static_cast<float>(count + 1);
        result.leading = share;
        result.gap += share;
    }
    return result;
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
    std::vector<float> mainSizes(visible.size(), 0.0f);
    std::vector<float> growWeights(visible.size(), 0.0f);
    std::vector<float> shrinkWeights(visible.size(), 0.0f);
    float baseTotal = 0.0f;
    float growTotal = 0.0f;
    for (std::size_t index = 0; index < visible.size(); ++index)
    {
        const auto& child = *visible[index];
        const ViewLength& mainLength = horizontal
            ? child.width : child.height;
        const float intrinsic = horizontal
            ? IntrinsicWidth(child) : IntrinsicHeight(child);
        float innerSize = intrinsic;
        if (child.flexBasis.kind == ViewLengthKind::Fixed)
            innerSize = child.flexBasis.value;
        else if (mainLength.kind == ViewLengthKind::Fixed)
            innerSize = mainLength.value;
        else if (mainLength.kind == ViewLengthKind::Fill)
            innerSize = 0.0f;
        const float mainMargin = horizontal
            ? ViewHorizontalMargin(child) : ViewVerticalMargin(child);
        mainSizes[index] = (horizontal
            ? ConstrainWidth(child, innerSize)
            : ConstrainHeight(child, innerSize)) + mainMargin;
        baseTotal += mainSizes[index];
        growWeights[index] = child.flexGrow > 0.0f
            ? child.flexGrow :
            (mainLength.kind == ViewLengthKind::Fill ? 1.0f : 0.0f);
        growTotal += growWeights[index];
        shrinkWeights[index] = child.flexShrink *
            std::max(1.0f,
                mainSizes[index] - mainMargin);
    }

    const float availableForItems = std::max(0.0f, availableMain - gaps);
    const float freeSpace = availableForItems - baseTotal;
    if (freeSpace > 0.0f && growTotal > 0.0f)
    {
        for (std::size_t index = 0; index < visible.size(); ++index)
            if (growWeights[index] > 0.0f)
                mainSizes[index] += freeSpace *
                    (growWeights[index] / growTotal);
    }
    else if (freeSpace < 0.0f)
    {
        float overflow = -freeSpace;
        std::vector<bool> active(visible.size(), true);
        for (std::size_t pass = 0;
            pass < visible.size() && overflow > 0.001f; ++pass)
        {
            float weightTotal = 0.0f;
            for (std::size_t index = 0; index < visible.size(); ++index)
                if (active[index]) weightTotal += shrinkWeights[index];
            if (weightTotal <= 0.0f) break;

            const float requested = overflow;
            float reduced = 0.0f;
            for (std::size_t index = 0; index < visible.size(); ++index)
            {
                if (!active[index] || shrinkWeights[index] <= 0.0f)
                    continue;
                const ViewNode& child = *visible[index];
                const float minimumInner = horizontal
                    ? child.minimumWidth.value_or(0.0f)
                    : child.minimumHeight.value_or(0.0f);
                const float minimumOuter = minimumInner +
                    (horizontal ? ViewHorizontalMargin(child) :
                        ViewVerticalMargin(child));
                const float capacity = std::max(
                    0.0f, mainSizes[index] - minimumOuter);
                const float share = requested *
                    (shrinkWeights[index] / weightTotal);
                const float delta = std::min(capacity, share);
                mainSizes[index] -= delta;
                reduced += delta;
                if (capacity <= share + 0.001f)
                    active[index] = false;
            }
            if (reduced <= 0.001f) break;
            overflow = std::max(0.0f, overflow - reduced);
        }
    }

    std::vector<float> crossSizes(visible.size(), 0.0f);
    for (std::size_t index = 0; index < visible.size(); ++index)
    {
        ViewNode& child = *visible[index];
        const ViewLength& crossLength = horizontal
            ? child.height : child.width;
        const float intrinsicCross = horizontal
            ? IntrinsicHeight(child) : IntrinsicWidth(child);
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? node.alignItems : child.alignSelf;
        const float proposedCross = ResolveOuterCrossSize(child,
            crossLength, intrinsicCross, availableCross, alignment,
            !horizontal);
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
            horizontal ? mainSizes[index] : proposedCross,
            horizontal ? proposedCross : mainSizes[index]);
        mainSizes[index] = horizontal ? resolved.width : resolved.height;
        crossSizes[index] = horizontal ? resolved.height : resolved.width;
    }

    float used = gaps;
    for (float value : mainSizes) used += value;
    const AxisSpacing spacing = ResolveAxisSpacing(
        node.justifyContent, node.gap, availableMain,
        used, visible.size());
    const bool reversed = IsFlexContainer(node.type) &&
        IsFlexMainReversed(node);
    const float axisStart = horizontal ? content.x : content.y;
    float cursor = reversed
        ? axisStart + availableMain - spacing.leading
        : axisStart + spacing.leading;

    for (std::size_t index = 0; index < visible.size(); ++index)
    {
        ViewNode& child = *visible[index];
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? node.alignItems : child.alignSelf;
        const float crossOffset = AlignOffset(
            alignment, availableCross, crossSizes[index]);
        if (reversed) cursor -= mainSizes[index];
        ViewRect childFrame;
        if (horizontal)
            childFrame = { cursor, content.y + crossOffset,
                mainSizes[index], crossSizes[index] };
        else
            childFrame = { content.x + crossOffset, cursor,
                crossSizes[index], mainSizes[index] };
        LayoutNode(child, childFrame);
        if (reversed) cursor -= spacing.gap;
        else cursor += mainSizes[index] + spacing.gap;
    }
}

void LayoutWrappedFlex(ViewNode& node, const ViewRect& content,
    bool horizontal)
{
    struct Item
    {
        ViewNode* node = nullptr;
        float main = 0.0f;
        float cross = 0.0f;
        float grow = 0.0f;
        float shrink = 0.0f;
    };
    struct Line
    {
        std::vector<Item> items;
        float baseMain = 0.0f;
        float cross = 0.0f;
    };

    const float availableMain = horizontal ? content.width : content.height;
    const float availableCross = horizontal ? content.height : content.width;
    std::vector<Line> lines;
    for (auto& child : node.children)
    {
        if (!child.visible) continue;
        const ViewLength& mainLength = horizontal
            ? child.width : child.height;
        const float intrinsicMain = horizontal
            ? IntrinsicWidth(child) : IntrinsicHeight(child);
        float innerMain = intrinsicMain;
        if (child.flexBasis.kind == ViewLengthKind::Fixed)
            innerMain = child.flexBasis.value;
        else if (mainLength.kind == ViewLengthKind::Fixed)
            innerMain = mainLength.value;
        else if (mainLength.kind == ViewLengthKind::Fill)
            innerMain = 0.0f;
        const float mainMargin = horizontal
            ? ViewHorizontalMargin(child) : ViewVerticalMargin(child);
        const float baseMain = (horizontal
            ? ConstrainWidth(child, innerMain)
            : ConstrainHeight(child, innerMain)) + mainMargin;
        if (lines.empty() || (!lines.back().items.empty() &&
                lines.back().baseMain + node.gap + baseMain >
                    availableMain + 0.001f))
            lines.emplace_back();
        Line& line = lines.back();
        if (!line.items.empty()) line.baseMain += node.gap;
        line.baseMain += baseMain;
        line.items.push_back({ &child, baseMain, 0.0f,
            child.flexGrow > 0.0f ? child.flexGrow :
                (mainLength.kind == ViewLengthKind::Fill ? 1.0f : 0.0f),
            child.flexShrink * std::max(1.0f, baseMain - mainMargin) });
    }
    if (lines.empty()) return;

    for (Line& line : lines)
    {
        const float gaps = node.gap *
            static_cast<float>(line.items.size() - 1);
        float baseTotal = 0.0f;
        float growTotal = 0.0f;
        for (const Item& item : line.items)
        {
            baseTotal += item.main;
            growTotal += item.grow;
        }
        const float availableForItems = std::max(0.0f,
            availableMain - gaps);
        const float freeSpace = availableForItems - baseTotal;
        if (freeSpace > 0.0f && growTotal > 0.0f)
        {
            for (Item& item : line.items)
                if (item.grow > 0.0f)
                    item.main += freeSpace * (item.grow / growTotal);
        }
        else if (freeSpace < 0.0f)
        {
            float overflow = -freeSpace;
            std::vector<bool> active(line.items.size(), true);
            for (std::size_t pass = 0;
                pass < line.items.size() && overflow > 0.001f; ++pass)
            {
                float weightTotal = 0.0f;
                for (std::size_t index = 0;
                    index < line.items.size(); ++index)
                    if (active[index])
                        weightTotal += line.items[index].shrink;
                if (weightTotal <= 0.0f) break;

                const float requested = overflow;
                float reduced = 0.0f;
                for (std::size_t index = 0;
                    index < line.items.size(); ++index)
                {
                    Item& item = line.items[index];
                    if (!active[index] || item.shrink <= 0.0f) continue;
                    const float minimumInner = horizontal
                        ? item.node->minimumWidth.value_or(0.0f)
                        : item.node->minimumHeight.value_or(0.0f);
                    const float minimumOuter = minimumInner + (horizontal
                        ? ViewHorizontalMargin(*item.node)
                        : ViewVerticalMargin(*item.node));
                    const float capacity = std::max(
                        0.0f, item.main - minimumOuter);
                    const float share = requested *
                        (item.shrink / weightTotal);
                    const float delta = std::min(capacity, share);
                    item.main -= delta;
                    reduced += delta;
                    if (capacity <= share + 0.001f)
                        active[index] = false;
                }
                if (reduced <= 0.001f) break;
                overflow = std::max(0.0f, overflow - reduced);
            }
        }

        for (Item& item : line.items)
        {
            ViewNode& child = *item.node;
            const ViewLength& crossLength = horizontal
                ? child.height : child.width;
            const float intrinsicCross = horizontal
                ? IntrinsicHeight(child) : IntrinsicWidth(child);
            const float naturalAvailable = intrinsicCross + (horizontal
                ? ViewVerticalMargin(child) : ViewHorizontalMargin(child));
            const float naturalCross = ResolveOuterCrossSize(child,
                crossLength, intrinsicCross, naturalAvailable,
                ViewAlignment::Start, !horizontal);
            const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
                horizontal ? item.main : naturalCross,
                horizontal ? naturalCross : item.main);
            item.main = horizontal ? resolved.width : resolved.height;
            item.cross = horizontal ? resolved.height : resolved.width;
            line.cross = std::max(line.cross, item.cross);
        }
    }

    float lineGap = node.gap;
    float usedCross = lineGap * static_cast<float>(lines.size() - 1);
    for (const Line& line : lines) usedCross += line.cross;
    if (usedCross > availableCross && usedCross > 0.0f)
    {
        const float gaps = lineGap * static_cast<float>(lines.size() - 1);
        const float scale = std::max(0.0f, availableCross - gaps) /
            std::max(0.001f, usedCross - gaps);
        for (Line& line : lines) line.cross *= scale;
        usedCross = std::min(availableCross, usedCross);
    }

    if (availableCross > usedCross)
    {
        const float extra = availableCross - usedCross;
        if (node.alignContent == ViewContentAlignment::Stretch)
        {
            const float addition = extra / static_cast<float>(lines.size());
            for (Line& line : lines) line.cross += addition;
            usedCross = availableCross;
        }
    }

    const AxisSpacing crossSpacing = ResolveAxisSpacing(
        node.alignContent, lineGap, availableCross,
        usedCross, lines.size());
    const bool crossReversed = node.flexWrap == ViewFlexWrap::WrapReverse;
    const float crossStart = horizontal ? content.y : content.x;
    float crossCursor = crossReversed
        ? crossStart + availableCross - crossSpacing.leading
        : crossStart + crossSpacing.leading;
    const bool mainReversed = IsFlexMainReversed(node);
    for (Line& line : lines)
    {
        float usedMain = node.gap *
            static_cast<float>(line.items.size() - 1);
        for (const Item& item : line.items) usedMain += item.main;
        const AxisSpacing mainSpacing = ResolveAxisSpacing(
            node.justifyContent, node.gap, availableMain,
            usedMain, line.items.size());
        const float mainStart = horizontal ? content.x : content.y;
        float mainCursor = mainReversed
            ? mainStart + availableMain - mainSpacing.leading
            : mainStart + mainSpacing.leading;
        if (crossReversed) crossCursor -= line.cross;
        const float lineCrossStart = crossCursor;

        for (Item& item : line.items)
        {
            ViewNode& child = *item.node;
            const ViewAlignment alignment =
                child.alignSelf == ViewAlignment::Auto
                ? node.alignItems : child.alignSelf;
            const ViewLength& crossLength = horizontal
                ? child.height : child.width;
            const float intrinsicCross = horizontal
                ? IntrinsicHeight(child) : IntrinsicWidth(child);
            const float proposedCross = ResolveOuterCrossSize(child,
                crossLength, intrinsicCross, line.cross, alignment,
                !horizontal);
            const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
                horizontal ? item.main : proposedCross,
                horizontal ? proposedCross : item.main);
            const float resolvedMain = horizontal
                ? resolved.width : resolved.height;
            const float resolvedCross = horizontal
                ? resolved.height : resolved.width;
            const float crossOffset = AlignOffset(
                alignment, line.cross, resolvedCross);
            if (mainReversed) mainCursor -= resolvedMain;
            if (horizontal)
                LayoutNode(child, { mainCursor,
                    lineCrossStart + crossOffset,
                    resolvedMain, resolvedCross });
            else
                LayoutNode(child, { lineCrossStart + crossOffset,
                    mainCursor, resolvedCross, resolvedMain });
            if (mainReversed) mainCursor -= mainSpacing.gap;
            else mainCursor += resolvedMain + mainSpacing.gap;
        }
        if (crossReversed) crossCursor -= crossSpacing.gap;
        else crossCursor += line.cross + crossSpacing.gap;
    }
}

void LayoutGrid(ViewNode& node, const ViewRect& content)
{
    std::vector<ViewNode*> visible;
    visible.reserve(node.children.size());
    for (auto& child : node.children)
        if (child.visible) visible.push_back(&child);
    if (visible.empty()) return;

    const std::size_t columns = node.columns;
    std::size_t rows = node.rowTracks.size();
    for (const ViewNode* child : visible)
        rows = std::max(rows, child->resolvedGridRow + child->rowSpan);
    if (rows == 0) return;
    const float columnGap = node.columnGap.value_or(node.gap);
    float rowGap = node.rowGap.value_or(node.gap);
    std::vector<float> columnWidths;
    if (node.columnTracks.empty())
    {
        const float cellWidth = std::max(0.0f,
            (content.width - columnGap * static_cast<float>(columns - 1)) /
                static_cast<float>(columns));
        columnWidths.assign(columns, cellWidth);
    }
    else
        columnWidths = ResolveGridTrackSizes(node, node.columnTracks,
            columns, true, columnGap, content.width);

    std::vector<float> rowHeights;
    if (!node.rowTracks.empty())
        rowHeights = ResolveGridTrackSizes(node, node.rowTracks,
            rows, false, rowGap, content.height);
    else
    {
        rowHeights.assign(rows, 0.0f);
        for (const ViewNode* child : visible)
        {
            float current = rowGap *
                static_cast<float>(child->rowSpan - 1);
            for (std::size_t row = child->resolvedGridRow;
                row < child->resolvedGridRow + child->rowSpan; ++row)
                current += rowHeights[row];
            const float deficit = std::max(
                0.0f, OuterIntrinsicHeight(*child) - current);
            const float addition = deficit /
                static_cast<float>(child->rowSpan);
            for (std::size_t row = child->resolvedGridRow;
                row < child->resolvedGridRow + child->rowSpan; ++row)
                rowHeights[row] += addition;
        }
    }

    float rowsHeight = 0.0f;
    for (float height : rowHeights) rowsHeight += height;
    const float gapsHeight = rowGap * static_cast<float>(rows - 1);
    const float availableRowsHeight = std::max(
        0.0f, content.height - gapsHeight);
    if (node.rowTracks.empty() &&
        rowsHeight > availableRowsHeight && rowsHeight > 0.0f)
    {
        const float scale = availableRowsHeight / rowsHeight;
        for (float& height : rowHeights) height *= scale;
        rowsHeight = availableRowsHeight;
    }

    const float usedHeight = rowsHeight + gapsHeight;
    const AxisSpacing rowSpacing = ResolveAxisSpacing(
        node.justifyContent, rowGap, content.height,
        usedHeight, rows);
    float y = content.y + rowSpacing.leading;
    rowGap = rowSpacing.gap;

    std::vector<float> rowOffsets(rows, y);
    for (std::size_t row = 1; row < rows; ++row)
        rowOffsets[row] = rowOffsets[row - 1] +
            rowHeights[row - 1] + rowGap;

    std::vector<float> columnOffsets(columns, content.x);
    for (std::size_t column = 1; column < columns; ++column)
        columnOffsets[column] = columnOffsets[column - 1] +
            columnWidths[column - 1] + columnGap;

    for (ViewNode* childPointer : visible)
    {
        ViewNode& child = *childPointer;
        const std::size_t column = child.resolvedGridColumn;
        const std::size_t row = child.resolvedGridRow;
        float spannedWidth = columnGap *
            static_cast<float>(child.columnSpan - 1);
        for (std::size_t currentColumn = column;
            currentColumn < column + child.columnSpan; ++currentColumn)
            spannedWidth += columnWidths[currentColumn];
        float spannedHeight = rowGap *
            static_cast<float>(child.rowSpan - 1);
        for (std::size_t currentRow = row;
            currentRow < row + child.rowSpan; ++currentRow)
            spannedHeight += rowHeights[currentRow];
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? node.alignItems : child.alignSelf;
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
            ResolveOuterCrossSize(child, child.width, IntrinsicWidth(child),
                spannedWidth, alignment, true),
            ResolveOuterCrossSize(child, child.height, IntrinsicHeight(child),
                spannedHeight, alignment, false));
        const float x = columnOffsets[column] +
            AlignOffset(alignment, spannedWidth, resolved.width);
        LayoutNode(child, { x,
            rowOffsets[row] + AlignOffset(
                alignment, spannedHeight, resolved.height),
            resolved.width, resolved.height });
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
        float width = OuterIntrinsicWidth(child);
        if (child.width.kind == ViewLengthKind::Fixed)
            width = child.width.value + ViewHorizontalMargin(child);
        else if (child.width.kind == ViewLengthKind::Fill)
            width = content.width;
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
            std::clamp(width, 0.0f, content.width),
            OuterIntrinsicHeight(child));
        width = resolved.width;
        const float height = resolved.height;
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
        const AxisSpacing spacing = ResolveAxisSpacing(
            node.justifyContent, columnGap, content.width,
            line.width, line.items.size());
        float x = content.x + spacing.leading;
        for (const Item& item : line.items)
        {
            ViewNode& child = *item.node;
            const ViewAlignment alignment =
                child.alignSelf == ViewAlignment::Auto
                ? node.alignItems : child.alignSelf;
            const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
                item.width, ResolveOuterCrossSize(child, child.height,
                    std::max(0.0f, item.intrinsicHeight -
                        ViewVerticalMargin(child)),
                    line.height, alignment, false));
            LayoutNode(child, { x,
                y + AlignOffset(alignment, line.height, resolved.height),
                resolved.width, resolved.height });
            x += resolved.width + spacing.gap;
        }
        y += line.height + rowGap;
    }
}

void LayoutScroll(ViewNode& node, const ViewRect& content)
{
    if (node.children.empty() || !node.children.front().visible) return;
    ViewNode& child = node.children.front();
    if (node.orientation == ViewOrientation::Vertical)
    {
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? ViewAlignment::Stretch : child.alignSelf;
        const float proposedWidth = ResolveOuterCrossSize(child, child.width,
            IntrinsicWidth(child), content.width, alignment, true);
        float height = child.height.kind == ViewLengthKind::Fixed
            ? child.height.value + ViewVerticalMargin(child) :
                OuterIntrinsicHeight(child);
        if (child.height.kind == ViewLengthKind::Fill)
            height = std::max(height, content.height);
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(
            child, proposedWidth, height);
        LayoutNode(child, { content.x + AlignOffset(alignment,
            content.width, resolved.width), content.y,
            resolved.width, resolved.height });
    }
    else
    {
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? ViewAlignment::Stretch : child.alignSelf;
        const float proposedHeight = ResolveOuterCrossSize(child, child.height,
            IntrinsicHeight(child), content.height, alignment, false);
        float width = child.width.kind == ViewLengthKind::Fixed
            ? child.width.value + ViewHorizontalMargin(child) :
                OuterIntrinsicWidth(child);
        if (child.width.kind == ViewLengthKind::Fill)
            width = std::max(width, content.width);
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(
            child, width, proposedHeight);
        LayoutNode(child, { content.x, content.y + AlignOffset(alignment,
            content.height, resolved.height),
            resolved.width, resolved.height });
    }
}

void LayoutVirtualCollection(ViewNode& node, const ViewRect& content)
{
    if (node.children.empty()) return;
    node.virtualMeasuredExtents.clear();
    node.virtualMeasuredIndices.clear();
    if (node.estimatedItemSize)
    {
        float cursor = 0.0f;
        std::size_t previousIndex = 0;
        std::string rangeError;
        const bool horizontal = node.orientation ==
            ViewOrientation::Horizontal;
        const float mainGap = VirtualCollectionMainGap(node);
        for (std::size_t childIndex = 0;
            childIndex < node.children.size(); ++childIndex)
        {
            ViewNode& child = node.children[childIndex];
            if (!child.visible) continue;
            const std::size_t itemIndex =
                node.virtualChildIndices[childIndex];
            if (previousIndex == 0 || itemIndex != previousIndex + 1)
            {
                if (!ComputeViewVariableVirtualItemStart(node.itemCount,
                        *node.estimatedItemSize, mainGap, itemIndex,
                        node.virtualMeasurements, cursor, rangeError))
                    return;
            }
            ResolvedNodeSize resolved;
            float measuredExtent = 0.0f;
            if (horizontal)
            {
                const float intrinsic = child.width.kind ==
                    ViewLengthKind::Fixed
                    ? child.width.value + ViewHorizontalMargin(child)
                    : std::max(1.0f, OuterIntrinsicWidth(child));
                resolved = ResolveOuterNodeSize(
                    child, intrinsic, content.height);
                measuredExtent = std::max(1.0f, resolved.width);
                LayoutNode(child, { content.x + cursor, content.y,
                    measuredExtent, resolved.height });
            }
            else
            {
                const float intrinsic = child.height.kind ==
                    ViewLengthKind::Fixed
                    ? child.height.value + ViewVerticalMargin(child)
                    : std::max(1.0f, OuterIntrinsicHeight(child));
                resolved = ResolveOuterNodeSize(
                    child, content.width, intrinsic);
                measuredExtent = std::max(1.0f, resolved.height);
                LayoutNode(child, { content.x, content.y + cursor,
                    resolved.width, measuredExtent });
            }
            node.virtualMeasuredExtents.push_back(measuredExtent);
            node.virtualMeasuredIndices.push_back(itemIndex);
            cursor += measuredExtent + mainGap;
            previousIndex = itemIndex;
        }
        return;
    }
    const std::size_t columns = node.type == ViewNodeType::VirtualGrid
        ? node.columns : 1;
    if (columns == 0 || node.firstIndex == 0) return;
    const float columnGap = node.columnGap.value_or(node.gap);
    const float rowGap = node.rowGap.value_or(node.gap);
    if (node.type == ViewNodeType::VirtualList &&
        node.orientation == ViewOrientation::Horizontal)
    {
        for (std::size_t childIndex = 0;
            childIndex < node.children.size(); ++childIndex)
        {
            ViewNode& child = node.children[childIndex];
            if (!child.visible) continue;
            const std::size_t itemIndex =
                node.virtualChildIndices[childIndex];
            const ResolvedNodeSize resolved = ResolveOuterNodeSize(
                child, node.itemExtent, content.height);
            LayoutNode(child, {
                content.x + static_cast<float>(itemIndex - 1) *
                    (node.itemExtent + columnGap),
                content.y, resolved.width, resolved.height,
            });
        }
        return;
    }
    const float cellWidth = std::max(0.0f,
        (content.width - columnGap * static_cast<float>(columns - 1)) /
            static_cast<float>(columns));
    for (std::size_t childIndex = 0;
        childIndex < node.children.size(); ++childIndex)
    {
        ViewNode& child = node.children[childIndex];
        if (!child.visible) continue;
        const std::size_t itemIndex = node.virtualChildIndices[childIndex];
        const std::size_t zeroBased = itemIndex - 1;
        const std::size_t row = zeroBased / columns;
        const std::size_t column = zeroBased % columns;
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(
            child, cellWidth, node.itemExtent);
        LayoutNode(child, {
            content.x + static_cast<float>(column) *
                (cellWidth + columnGap),
            content.y + static_cast<float>(row) *
                (node.itemExtent + rowGap),
            resolved.width,
            resolved.height,
        });
    }
}

void LayoutNode(ViewNode& node, const ViewRect& frame)
{
    const ViewEdgeInsets margin = ResolveInsets(
        node.margin, frame.width, frame.height);
    const ResolvedNodeSize resolved = ResolveNodeSize(
        node, std::max(0.0f, frame.width - HorizontalInsets(margin)),
        std::max(0.0f, frame.height - VerticalInsets(margin)));
    node.frame = { frame.x + margin.left + node.offsetX,
        frame.y + margin.top + node.offsetY,
        resolved.width, resolved.height };
    node.clipFrame.reset();
    node.scrollbarFrame.reset();
    node.scrollOffset = 0.0f;
    node.scrollViewportExtent = 0.0f;
    node.scrollContentExtent = 0.0f;
    node.stickyPresented = false;
    const ViewRect content = ContentRect(node);
    const auto layoutOverlayChildren = [&]() {
        for (auto& child : node.children)
        {
            if (!child.visible) continue;
            const ViewAlignment alignment =
                child.alignSelf == ViewAlignment::Auto
                ? ViewAlignment::Stretch : child.alignSelf;
            const ResolvedNodeSize childSize = ResolveOuterNodeSize(child,
                ResolveOuterCrossSize(child, child.width,
                    IntrinsicWidth(child),
                    content.width, alignment, true),
                ResolveOuterCrossSize(child, child.height,
                    IntrinsicHeight(child),
                    content.height, alignment, false));
            LayoutNode(child, {
                content.x + AlignOffset(alignment,
                    content.width, childSize.width),
                content.y + AlignOffset(alignment,
                    content.height, childSize.height),
                childSize.width, childSize.height });
        }
    };
    if (HasCollectionPlaceholder(node))
        layoutOverlayChildren();
    else if (IsFlexContainer(node.type))
    {
        const bool horizontal = IsHorizontalFlex(node);
        if (node.flexWrap != ViewFlexWrap::NoWrap)
            LayoutWrappedFlex(node, content, horizontal);
        else
            LayoutLinear(node, content, horizontal);
    }
    else if (node.type == ViewNodeType::List)
        LayoutLinear(node, content,
            node.orientation == ViewOrientation::Horizontal);
    else if (IsVirtualCollection(node.type))
        LayoutVirtualCollection(node, content);
    else if (IsGridContainer(node.type))
        LayoutGrid(node, content);
    else if (node.type == ViewNodeType::Flow)
        LayoutFlow(node, content);
    else if (node.type == ViewNodeType::Scroll)
        LayoutScroll(node, content);
    else if (node.type == ViewNodeType::Box ||
        node.type == ViewNodeType::Stack ||
        node.type == ViewNodeType::ListItem ||
        node.type == ViewNodeType::SlotSurface ||
        node.type == ViewNodeType::SlotItem)
    {
        layoutOverlayChildren();
    }
    if (node.clipChildren || node.overflow == ViewOverflow::Clip)
        node.clipFrame = ContentRect(node);
}

void CaptureLayoutTransitionFrames(ViewNode& node,
    const std::optional<ViewRect>& parentFrame) noexcept
{
    node.layoutTransitionFrame = node.frame;
    if (parentFrame)
    {
        node.layoutTransitionFrame.x -= parentFrame->x;
        node.layoutTransitionFrame.y -= parentFrame->y;
    }
    const ViewRect currentFrame = node.frame;
    for (auto& child : node.children)
        CaptureLayoutTransitionFrames(child, currentFrame);
}

}

bool ResolveGridPlacements(ViewNode& node, std::string& error)
{
    constexpr std::size_t maximumTracks = 64;
    if (IsPositionedGridContainer(node.type) &&
        !HasCollectionPlaceholder(node))
    {
        std::vector<std::vector<bool>> occupied;
        const auto ensureRows = [&](std::size_t count) {
            while (occupied.size() < count)
                occupied.emplace_back(node.columns, false);
        };
        const auto fits = [&](std::size_t row, std::size_t column,
                              std::size_t rowSpan,
                              std::size_t columnSpan) {
            if (row + rowSpan > maximumTracks ||
                column + columnSpan > node.columns)
                return false;
            for (std::size_t currentRow = row;
                currentRow < row + rowSpan; ++currentRow)
            {
                if (currentRow >= occupied.size()) continue;
                for (std::size_t currentColumn = column;
                    currentColumn < column + columnSpan; ++currentColumn)
                    if (occupied[currentRow][currentColumn]) return false;
            }
            return true;
        };
        const auto occupy = [&](std::size_t row, std::size_t column,
                                std::size_t rowSpan,
                                std::size_t columnSpan) {
            ensureRows(row + rowSpan);
            for (std::size_t currentRow = row;
                currentRow < row + rowSpan; ++currentRow)
                for (std::size_t currentColumn = column;
                    currentColumn < column + columnSpan; ++currentColumn)
                    occupied[currentRow][currentColumn] = true;
        };

        for (auto& child : node.children)
        {
            child.resolvedGridColumn = 0;
            child.resolvedGridRow = 0;
            const std::size_t columnSpan = child.columnSpan;
            const std::size_t rowSpan = child.rowSpan;
            if (columnSpan > node.columns || rowSpan > maximumTracks ||
                (child.gridColumn && *child.gridColumn > node.columns) ||
                (child.gridRow && *child.gridRow > maximumTracks) ||
                (child.gridColumn &&
                    *child.gridColumn - 1 + columnSpan > node.columns) ||
                (child.gridRow &&
                    *child.gridRow - 1 + rowSpan > maximumTracks))
            {
                error = "grid item placement exceeds the 64-track grid bounds";
                return false;
            }
            if (!child.visible) continue;

            const std::size_t firstRow = child.gridRow
                ? *child.gridRow - 1 : 0;
            const std::size_t lastRow = child.gridRow
                ? firstRow : maximumTracks - rowSpan;
            const std::size_t firstColumn = child.gridColumn
                ? *child.gridColumn - 1 : 0;
            const std::size_t lastColumn = child.gridColumn
                ? firstColumn : node.columns - columnSpan;
            bool found = false;
            for (std::size_t row = firstRow;
                row <= lastRow && !found; ++row)
            {
                for (std::size_t column = firstColumn;
                    column <= lastColumn; ++column)
                {
                    if (!fits(row, column, rowSpan, columnSpan)) continue;
                    child.resolvedGridRow = row;
                    child.resolvedGridColumn = column;
                    occupy(row, column, rowSpan, columnSpan);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                error = child.gridRow && child.gridColumn
                    ? "grid item placement overlaps an occupied cell"
                    : "grid auto-placement exceeds the 64-track limit";
                return false;
            }
        }
    }
    for (auto& child : node.children)
        if (!ResolveGridPlacements(child, error)) return false;
    return true;
}

void LayoutViewTree(ViewNode& root, const ViewRect& frame)
{
    LayoutNode(root, frame);
    CaptureLayoutTransitionFrames(root, std::nullopt);
}
}

ViewRect ViewNodeContentRect(const ViewNode& node) noexcept
{
    return detail::ContentRect(node);
}
}
