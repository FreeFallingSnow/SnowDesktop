#pragma once

#include "widget_view_tree.h"
#include "widget/api/widget_view_contract.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace snowdesktop::widget_runtime::detail
{
inline constexpr float MaximumDimension = 100000.0f;
inline constexpr float MaximumScrollExtent = 1000000.0f;

inline bool FiniteInRange(float value, float minimum, float maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

inline float ConstrainDimension(float value, const std::optional<float>& minimum,
    const std::optional<float>& maximum) noexcept
{
    if (minimum) value = std::max(value, *minimum);
    if (maximum) value = std::min(value, *maximum);
    return std::max(0.0f, value);
}

inline float ConstrainWidth(const ViewNode& node, float value) noexcept
{
    return ConstrainDimension(
        value, node.minimumWidth, node.maximumWidth);
}

inline float ConstrainHeight(const ViewNode& node, float value) noexcept
{
    return ConstrainDimension(
        value, node.minimumHeight, node.maximumHeight);
}

inline bool IsIconNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Icon ||
        type == ViewNodeType::IconButton;
}

inline bool IsCheckControlNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Toggle ||
        type == ViewNodeType::Checkbox;
}

inline bool IsInputNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::TextInput ||
        type == ViewNodeType::TextArea ||
        type == ViewNodeType::SearchBox ||
        type == ViewNodeType::NumberInput;
}

inline bool IsChoiceNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::RadioGroup ||
        type == ViewNodeType::Select;
}

inline bool IsDataSeriesNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Sparkline ||
        type == ViewNodeType::LineChart ||
        type == ViewNodeType::BarChart ||
        type == ViewNodeType::Waveform ||
        type == ViewNodeType::Spectrum;
}

inline bool IsGridContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Grid || type == ViewNodeType::GridList ||
        type == ViewNodeType::VirtualGrid;
}

inline bool IsPositionedGridContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Grid || type == ViewNodeType::GridList;
}

inline bool IsFlexContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Row || type == ViewNodeType::Column;
}

inline bool IsVirtualCollection(ViewNodeType type) noexcept
{
    return type == ViewNodeType::VirtualList ||
        type == ViewNodeType::VirtualGrid;
}

inline float VirtualCollectionMainGap(const ViewNode& node) noexcept
{
    return node.orientation == ViewOrientation::Horizontal
        ? node.columnGap.value_or(node.gap)
        : node.rowGap.value_or(node.gap);
}

inline bool IsCollectionContainer(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract &&
        contract->childPolicy == ViewChildPolicy::Collection;
}

inline bool HasCollectionPlaceholder(const ViewNode& node) noexcept
{
    return IsCollectionContainer(node.type) &&
        node.collectionContent != ViewCollectionContent::Items;
}

inline bool IsScrollContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Scroll || IsVirtualCollection(type);
}

inline bool IsNodeKeyboardFocusable(const ViewNode& node) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(node.type);
    return node.focusable.value_or(
        contract && contract->keyboardFocusable);
}
}
