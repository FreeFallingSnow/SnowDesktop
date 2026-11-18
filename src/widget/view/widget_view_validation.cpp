#include "widget_view_validation.h"
#include "widget_view_node_rules.h"
#include "widget_view_transition_validation.h"
#include "widget_view_tree.h"
#include "widget/api/widget_view_contract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace snowdesktop::widget_runtime::detail
{
namespace
{
bool IsValidLocaleTag(std::string_view value) noexcept
{
    if (value.empty()) return true;
    if (value.size() > 85 || value.front() == '-' || value.back() == '-')
        return false;
    std::size_t subtagLength = 0;
    std::size_t subtagIndex = 0;
    for (const unsigned char character : value)
    {
        if (character == '-')
        {
            if (subtagLength == 0 || subtagLength > 8) return false;
            ++subtagIndex;
            subtagLength = 0;
            continue;
        }
        const bool alpha = (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if ((!alpha && !digit) || (subtagIndex == 0 && !alpha))
            return false;
        ++subtagLength;
    }
    return subtagLength > 0 && subtagLength <= 8 &&
        (subtagIndex > 0 || subtagLength >= 2);
}

bool ValidateLength(const ViewLength& length) noexcept
{
    return length.kind != ViewLengthKind::Fixed ||
        FiniteInRange(length.value, 0.0f, MaximumDimension);
}

bool IsSingleUtf8Scalar(std::string_view value) noexcept
{
    if (value.empty()) return false;
    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(value[index]);
    };
    const unsigned char first = byte(0);
    std::size_t length = 0;
    std::uint32_t scalar = 0;
    if (first <= 0x7F)
    {
        length = 1;
        scalar = first;
    }
    else if (first >= 0xC2 && first <= 0xDF)
    {
        length = 2;
        scalar = first & 0x1F;
    }
    else if (first >= 0xE0 && first <= 0xEF)
    {
        length = 3;
        scalar = first & 0x0F;
    }
    else if (first >= 0xF0 && first <= 0xF4)
    {
        length = 4;
        scalar = first & 0x07;
    }
    else return false;
    if (value.size() != length) return false;
    for (std::size_t index = 1; index < length; ++index)
    {
        const unsigned char next = byte(index);
        if ((next & 0xC0) != 0x80) return false;
        scalar = (scalar << 6) | (next & 0x3F);
    }
    const std::uint32_t minimum = length == 1 ? 0 :
        length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
    return scalar >= minimum && scalar <= 0x10FFFF &&
        !(scalar >= 0xD800 && scalar <= 0xDFFF);
}

bool IsControlledNode(ViewNodeType type) noexcept
{
    return IsCheckControlNode(type) ||
        IsChoiceNode(type) || IsInputNode(type) ||
        type == ViewNodeType::Slider ||
        type == ViewNodeType::MonthCalendar;
}

bool IsLeafNode(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract && contract->childPolicy == ViewChildPolicy::None;
}

bool ValidateNode(const ViewNode& node, std::size_t depth,
    std::size_t& nodes, std::size_t& textBytes,
    std::size_t& seriesPoints, std::size_t& collectionItems,
    std::unordered_set<std::string>& keys,
    std::unordered_set<char>& accessKeys,
    std::unordered_set<std::string>& resources,
    std::optional<ViewNodeType> parentType, std::string& error)
{
    const bool accessibleNameDeclared =
        !node.accessibilityLabel.empty() ||
        !node.accessibilityLabelledBy.empty();
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
    if (node.debugName.size() > ViewTreeLimits::MaximumDebugNameBytes ||
        node.testId.size() > ViewTreeLimits::MaximumTestIdBytes ||
        (!node.debugName.empty() && std::any_of(
            node.debugName.begin(), node.debugName.end(),
            [](unsigned char value) {
                return value < 0x20 || value == 0x7F;
            })) ||
        (!node.testId.empty() && !std::all_of(
            node.testId.begin(), node.testId.end(), [](unsigned char value) {
                return (value >= 'a' && value <= 'z') ||
                    (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9') ||
                    value == '.' || value == '_' || value == '-' ||
                    value == ':' || value == '/';
            })))
    {
        error = "view debugName/testId exceeds its bound or contains unsupported characters";
        return false;
    }
    const auto validStyle = [](const ViewStyle& style) {
        return (!style.borderWidth || FiniteInRange(
                    *style.borderWidth, 0.0f, 4096.0f)) &&
            (!style.cornerRadius || FiniteInRange(
                    *style.cornerRadius, 0.0f, 4096.0f)) &&
            (!style.opacity || FiniteInRange(*style.opacity, 0.0f, 1.0f));
    };
    if (!ValidateViewNodeTransitions(node, error)) return false;
    if (node.maximumLines > 64)
    {
        error = "view maxLines must be between 0 and 64";
        return false;
    }
    if (node.fontWeight != 0 &&
        (node.fontWeight < 100 || node.fontWeight > 900 ||
            node.fontWeight % 100 != 0))
    {
        error = "view fontWeight must be 0 or a multiple of 100 from 100 to 900";
        return false;
    }
    if (node.verticalAlign != ViewAlignment::Start &&
        node.verticalAlign != ViewAlignment::Center &&
        node.verticalAlign != ViewAlignment::End)
    {
        error = "view verticalAlign must be start, center, or end";
        return false;
    }
    if (!IsValidLocaleTag(node.locale))
    {
        error = "view locale must be an empty or bounded BCP 47 language tag";
        return false;
    }
    if (node.indeterminate && node.type == ViewNodeType::Checkbox &&
        node.checked)
    {
        error = "view indeterminate requires a checkbox with checked=false";
        return false;
    }
    if (node.indeterminate &&
        node.type != ViewNodeType::Checkbox &&
        node.type != ViewNodeType::ProgressBar &&
        node.type != ViewNodeType::ProgressRing)
    {
        error = "view indeterminate is only valid for checkbox, progressBar, and progressRing nodes";
        return false;
    }
    if (node.required &&
        !IsInputNode(node.type) && node.type != ViewNodeType::Select)
    {
        error = "view required is only valid for input and select nodes";
        return false;
    }
    if (node.tabIndex && (*node.tabIndex < -1 || *node.tabIndex > 32767))
    {
        error = "view tabIndex must be between -1 and 32767";
        return false;
    }
    const ViewNodeContract* nodeContract = FindViewNodeContract(node.type);
    const bool keyboardFocusable = IsNodeKeyboardFocusable(node);
    if (!node.accessKey.empty())
    {
        if (node.accessKey.size() != 1 ||
            !((node.accessKey[0] >= 'A' && node.accessKey[0] <= 'Z') ||
                (node.accessKey[0] >= 'a' && node.accessKey[0] <= 'z') ||
                (node.accessKey[0] >= '0' && node.accessKey[0] <= '9')))
        {
            error = "view accessKey must be one ASCII letter or digit";
            return false;
        }
        if (!keyboardFocusable)
        {
            error = "view accessKey requires a focusable node";
            return false;
        }
        const char normalized = node.accessKey[0] >= 'a' &&
                node.accessKey[0] <= 'z'
            ? static_cast<char>(node.accessKey[0] - 'a' + 'A')
            : node.accessKey[0];
        if (!accessKeys.insert(normalized).second)
        {
            error = std::string("duplicate view accessKey: ") + normalized;
            return false;
        }
    }
    if (node.acceleratorText.size() > 64)
    {
        error = "view acceleratorText must contain at most 64 bytes";
        return false;
    }
    if (node.tabIndex && !keyboardFocusable)
    {
        error = "view tabIndex requires a focusable node";
        return false;
    }
    if (node.focusable.value_or(false) &&
        (!nodeContract || nodeContract->uiaControlType.empty()))
    {
        error = "view focusable=true requires a semantic node";
        return false;
    }
    if (node.focusable.value_or(false) &&
        node.accessibilityLabel.empty() && node.text.empty() &&
        node.alt.empty() && node.accessibilityLabelledBy.empty())
    {
        error = "view focusable=true requires accessible text, accessibility.label, or accessibility.labelledBy";
        return false;
    }
    if (!IsFlexContainer(node.type) &&
        (node.flexDirection != ViewFlexDirection::Auto ||
            node.flexWrap != ViewFlexWrap::NoWrap ||
            node.alignContent != ViewContentAlignment::Stretch))
    {
        error = "flexDirection, flexWrap, and alignContent are reserved for row and column nodes";
        return false;
    }
    if ((node.offsetX != 0.0f || node.offsetY != 0.0f ||
            node.zIndex != 0) &&
        (!parentType || *parentType != ViewNodeType::Stack))
    {
        error = "view offset and zIndex are only valid for direct stack children";
        return false;
    }
    if ((node.gridColumn || node.gridRow || node.columnSpan != 1 ||
            node.rowSpan != 1) &&
        (!parentType || !IsPositionedGridContainer(*parentType)))
    {
        error = "grid placement properties are only valid for direct grid or gridList children";
        return false;
    }
    if ((node.clipChildren || node.overflow == ViewOverflow::Clip) &&
        IsLeafNode(node.type))
    {
        error = "view clip is only valid for container nodes";
        return false;
    }
    if (node.shadow &&
        (!FiniteInRange(node.shadow->blur, 0.0f, 64.0f) ||
            !FiniteInRange(node.shadow->offsetX, -4096.0f, 4096.0f) ||
            !FiniteInRange(node.shadow->offsetY, -4096.0f, 4096.0f) ||
            !FiniteInRange(node.shadow->alpha, 0.0f, 1.0f)))
    {
        error = "view shadow values must be finite and bounded";
        return false;
    }
    if (!ValidateLength(node.width) || !ValidateLength(node.height) ||
        !ValidateLength(node.flexBasis) ||
        node.flexBasis.kind == ViewLengthKind::Fill ||
        (node.minimumWidth && !FiniteInRange(
            *node.minimumWidth, 0.0f, MaximumDimension)) ||
        (node.maximumWidth && !FiniteInRange(
            *node.maximumWidth, 0.0f, MaximumDimension)) ||
        (node.minimumHeight && !FiniteInRange(
            *node.minimumHeight, 0.0f, MaximumDimension)) ||
        (node.maximumHeight && !FiniteInRange(
            *node.maximumHeight, 0.0f, MaximumDimension)) ||
        (node.minimumWidth && node.maximumWidth &&
            *node.minimumWidth > *node.maximumWidth) ||
        (node.minimumHeight && node.maximumHeight &&
            *node.minimumHeight > *node.maximumHeight) ||
        (node.aspectRatio && !FiniteInRange(
            *node.aspectRatio, 0.01f, 100.0f)) ||
        !FiniteInRange(node.margin.top, 0.0f, 4096.0f) ||
        !FiniteInRange(node.margin.right, 0.0f, 4096.0f) ||
        !FiniteInRange(node.margin.bottom, 0.0f, 4096.0f) ||
        !FiniteInRange(node.margin.left, 0.0f, 4096.0f) ||
        !FiniteInRange(node.padding.top, 0.0f, 4096.0f) ||
        !FiniteInRange(node.padding.right, 0.0f, 4096.0f) ||
        !FiniteInRange(node.padding.bottom, 0.0f, 4096.0f) ||
        !FiniteInRange(node.padding.left, 0.0f, 4096.0f) ||
        !FiniteInRange(node.offsetX, -4096.0f, 4096.0f) ||
        !FiniteInRange(node.offsetY, -4096.0f, 4096.0f) ||
        node.zIndex < -1024 || node.zIndex > 1024 ||
        node.columnSpan == 0 || node.columnSpan > 64 ||
        node.rowSpan == 0 || node.rowSpan > 64 ||
        (node.gridColumn && (*node.gridColumn == 0 ||
            *node.gridColumn > 64)) ||
        (node.gridRow && (*node.gridRow == 0 || *node.gridRow > 64)) ||
        !FiniteInRange(node.gap, 0.0f, 4096.0f) ||
        (node.columnGap && !FiniteInRange(*node.columnGap, 0.0f, 4096.0f)) ||
        (node.rowGap && !FiniteInRange(*node.rowGap, 0.0f, 4096.0f)) ||
        !FiniteInRange(node.flexGrow, 0.0f, 1000.0f) ||
        !FiniteInRange(node.flexShrink, 0.0f, 1000.0f) ||
        !FiniteInRange(node.fontSize, 1.0f, 512.0f) ||
        (node.lineHeight &&
            !FiniteInRange(*node.lineHeight, 1.0f, 1024.0f)) ||
        !FiniteInRange(node.letterSpacing, -64.0f, 256.0f) ||
        !FiniteInRange(node.thickness, 0.5f, 4096.0f) ||
        !FiniteInRange(node.trackOpacity, 0.0f, 1.0f) ||
        !FiniteInRange(node.fillOpacity, 0.0f, 1.0f) ||
        !validStyle(node.style) || !validStyle(node.hoverStyle) ||
        !validStyle(node.pressedStyle) ||
        !validStyle(node.focusStyle) ||
        !validStyle(node.disabledStyle) ||
        !validStyle(node.validationStyle) ||
        !validStyle(node.checkedStyle) ||
        !validStyle(node.selectedStyle) ||
        !validStyle(node.dropStyle) ||
        !validStyle(node.todayStyle) ||
        !validStyle(node.adjacentStyle) ||
        !validStyle(node.eventStyle))
    {
        error = "view node dimensions and typography must be finite and bounded";
        return false;
    }
    if (node.aspectRatio)
    {
        const float ratio = *node.aspectRatio;
        const float feasibleMinimumWidth = std::max(
            node.minimumWidth.value_or(0.0f),
            node.minimumHeight.value_or(0.0f) * ratio);
        const float feasibleMaximumWidth = std::min(
            node.maximumWidth.value_or(MaximumDimension),
            node.maximumHeight.value_or(MaximumDimension) * ratio);
        if (feasibleMinimumWidth > feasibleMaximumWidth)
        {
            error = "aspectRatio conflicts with size constraints";
            return false;
        }
        if (node.width.kind == ViewLengthKind::Fixed &&
            node.height.kind == ViewLengthKind::Fixed)
        {
            const float width = ConstrainWidth(node, node.width.value);
            const float height = ConstrainHeight(node, node.height.value);
            if (height <= 0.0f || std::abs(width / height - ratio) >
                    std::max(0.001f, ratio * 0.001f))
            {
                error = "fixed width and height must match aspectRatio";
                return false;
            }
        }
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
    if (node.type == ViewNodeType::NumberInput &&
        (!FiniteInRange(node.minimum, -1.0e9f, 1.0e9f) ||
            !FiniteInRange(node.maximum, -1.0e9f, 1.0e9f) ||
            !FiniteInRange(node.value, -1.0e9f, 1.0e9f) ||
            !FiniteInRange(node.step, 0.000001f, 1.0e9f) ||
            node.minimum >= node.maximum || node.value < node.minimum ||
            node.value > node.maximum ||
            node.step > node.maximum - node.minimum))
    {
        error = "numberInput requires min < max, a value in range, and a positive bounded step";
        return false;
    }
    if (IsGridContainer(node.type) &&
        (node.columns == 0 || node.columns > 64))
    {
        error = "grid columns must be between 1 and 64";
        return false;
    }
    const auto validGridTrack = [](const ViewGridTrack& track) {
        if (track.kind == ViewGridTrackKind::Fixed)
            return FiniteInRange(track.value, 0.0f, MaximumDimension);
        if (track.kind == ViewGridTrackKind::Auto) return true;
        if (track.kind == ViewGridTrackKind::Fraction)
            return FiniteInRange(track.value, 0.000001f, 1000.0f);
        if (track.kind != ViewGridTrackKind::MinMax ||
            !FiniteInRange(track.minimum, 0.0f, MaximumDimension))
            return false;
        if (track.maximumKind == ViewGridTrackKind::Auto) return true;
        if (track.maximumKind == ViewGridTrackKind::Fixed)
            return FiniteInRange(track.maximumValue,
                track.minimum, MaximumDimension);
        return track.maximumKind == ViewGridTrackKind::Fraction &&
            FiniteInRange(track.maximumValue, 0.000001f, 1000.0f);
    };
    if ((!node.columnTracks.empty() &&
            (node.type != ViewNodeType::Grid &&
                node.type != ViewNodeType::GridList)) ||
        (!node.columnTracks.empty() &&
            node.columnTracks.size() != node.columns) ||
        node.columnTracks.size() > 64 ||
        !std::all_of(node.columnTracks.begin(), node.columnTracks.end(),
            validGridTrack))
    {
        error = "grid column tracks must be a valid 1 to 64 track definition";
        return false;
    }
    if ((!node.rowTracks.empty() &&
            node.type != ViewNodeType::Grid &&
            node.type != ViewNodeType::GridList) ||
        node.rowTracks.size() > 64 ||
        !std::all_of(node.rowTracks.begin(), node.rowTracks.end(),
            validGridTrack))
    {
        error = "grid row tracks must be a valid 1 to 64 track definition";
        return false;
    }
    if (!IsGridContainer(node.type) && node.columns != 1)
    {
        error = "grid columns are reserved for grid nodes";
        return false;
    }
    if (!IsGridContainer(node.type) &&
        node.type != ViewNodeType::Flow &&
        node.type != ViewNodeType::VirtualList &&
        (node.columnGap || node.rowGap))
    {
        error = "columnGap and rowGap are reserved for grid, flow, and virtual collection nodes";
        return false;
    }
    if (node.type == ViewNodeType::VirtualList &&
        ((node.orientation == ViewOrientation::Horizontal && node.rowGap) ||
         (node.orientation == ViewOrientation::Vertical && node.columnGap)))
    {
        error = "virtualList gap must match its main axis";
        return false;
    }
    if (!IsVirtualCollection(node.type) &&
        (node.itemCount != 0 || node.firstIndex != 0 ||
            node.itemExtent != 0.0f || node.estimatedItemSize ||
            node.virtualLayoutRevision != 0 || node.overscan != 2))
    {
        error = "virtual collection metadata is reserved for virtual nodes";
        return false;
    }
    if (node.type != ViewNodeType::VirtualList &&
        (!node.sectionHeaderIndices.empty() || node.stickyHeaderIndex))
    {
        error = "virtual section headers are reserved for virtualList";
        return false;
    }
    if ((node.initialScrollKey && node.type != ViewNodeType::Scroll) ||
        (node.initialScrollIndex && !IsVirtualCollection(node.type)))
    {
        error = "initial scroll targets are reserved for scroll containers";
        return false;
    }
    if (node.sticky && node.type != ViewNodeType::ListItem)
    {
        error = "sticky is reserved for listItem nodes";
        return false;
    }
    if (node.initialScrollKey &&
        (node.initialScrollKey->empty() ||
            node.initialScrollKey->size() > 128))
    {
        error = "initialScrollKey must contain 1 to 128 bytes";
        return false;
    }
    if (!IsScrollContainer(node.type) && !node.showScrollbar)
    {
        error = "showScrollbar is reserved for scroll containers";
        return false;
    }
    if (node.type == ViewNodeType::Scroll &&
        (node.children.size() != 1 || !node.children.front().visible))
    {
        error = "scroll nodes require exactly one child and it must be visible";
        return false;
    }
    if (IsCollectionContainer(node.type))
    {
        const bool itemContent = node.collectionContent ==
            ViewCollectionContent::Items;
        if (itemContent &&
            !std::all_of(node.children.begin(), node.children.end(),
                [](const ViewNode& child) {
                    return child.type == ViewNodeType::ListItem;
                }))
        {
            error = std::string(ViewNodeTypeName(node.type)) +
                " children must all be listItem nodes";
            return false;
        }
        if (!itemContent &&
            (node.children.size() != 1 || !node.children.front().visible))
        {
            error = "collection empty/loading content must be one visible node";
            return false;
        }
        if (node.collectionContent == ViewCollectionContent::Empty &&
            !node.selectedKeys.empty())
        {
            error = "empty collections cannot retain selectedKeys";
            return false;
        }
        if ((node.selectionMode == ViewSelectionMode::None &&
                !node.selectedKeys.empty()) ||
            (node.selectionMode == ViewSelectionMode::Single &&
                node.selectedKeys.size() > 1))
        {
            error = "collection selectedKeys do not match selectionMode";
            return false;
        }
        std::unordered_set<std::string> selectedKeys;
        for (const auto& key : node.selectedKeys)
        {
            if (key.empty() || key.size() > 128 ||
                !selectedKeys.insert(key).second)
            {
                error = "collection selectedKeys must be unique bounded item keys";
                return false;
            }
            if (textBytes > ViewTreeLimits::MaximumTotalTextBytes -
                    key.size())
            {
                error = "view tree collection key limit exceeded";
                return false;
            }
            textBytes += key.size();
        }
        if (itemContent && !IsVirtualCollection(node.type))
        {
            for (const auto& key : node.selectedKeys)
            {
                if (std::none_of(node.children.begin(), node.children.end(),
                        [&key](const ViewNode& child) {
                            return child.key == key;
                        }))
                {
                    error = "collection selectedKeys must reference direct listItem children";
                    return false;
                }
            }
        }
        if (itemContent &&
            node.selectionMode != ViewSelectionMode::None &&
            std::any_of(node.children.begin(), node.children.end(),
                [](const ViewNode& child) {
                    return child.events.contains("click") ||
                        child.events.contains("change");
                }))
        {
            error = "selectable collection items reserve click/change; use doubleClick for activation";
            return false;
        }
        if (std::any_of(node.children.begin(), node.children.end(),
                [](const ViewNode& child) { return child.sticky; }) &&
            (node.type != ViewNodeType::List ||
                node.orientation != ViewOrientation::Vertical))
        {
            error = "sticky listItem nodes require a vertical eager list";
            return false;
        }
    }
    else if (node.selectionMode != ViewSelectionMode::None ||
        !node.selectedKeys.empty() ||
        (node.collectionContent != ViewCollectionContent::Items &&
            !(node.type == ViewNodeType::SlotSurface &&
                node.collectionContent == ViewCollectionContent::Empty)))
    {
        error = "selectionMode and selectedKeys are reserved for collection nodes";
        return false;
    }
    if (IsVirtualCollection(node.type))
    {
        const bool variable = node.estimatedItemSize.has_value();
        if ((node.type == ViewNodeType::VirtualGrid && variable) ||
            (!variable && node.itemExtent <= 0.0f) ||
            (variable && (node.type != ViewNodeType::VirtualList ||
                node.itemExtent != 0.0f ||
                !FiniteInRange(*node.estimatedItemSize,
                    0.000001f, MaximumScrollExtent))) ||
            (!variable && node.virtualLayoutRevision != 0))
        {
            error = "virtualList requires one valid fixed or estimated item size; virtualGrid requires fixed itemExtent";
            return false;
        }
        if (node.type == ViewNodeType::VirtualGrid &&
            node.orientation != ViewOrientation::Vertical)
        {
            error = "virtualGrid nodes are vertical";
            return false;
        }
        if (node.orientation == ViewOrientation::Horizontal &&
            (!node.sectionHeaderIndices.empty() || node.stickyHeaderIndex))
        {
            error = "horizontal virtualList rejects section headers";
            return false;
        }
        const ViewLength& viewportLength =
            node.orientation == ViewOrientation::Horizontal
            ? node.width : node.height;
        if (viewportLength.kind == ViewLengthKind::Auto)
        {
            error = node.orientation == ViewOrientation::Horizontal
                ? "horizontal virtualList requires fixed or fill width"
                : "virtual collection nodes require fixed or fill height";
            return false;
        }
        if (node.itemCount > ViewTreeLimits::MaximumVirtualItemCount ||
            node.overscan > ViewTreeLimits::MaximumVirtualOverscan)
        {
            error = "virtual collection itemCount or overscan exceeds its limit";
            return false;
        }
        if (node.initialScrollIndex &&
            (*node.initialScrollIndex == 0 ||
                *node.initialScrollIndex > node.itemCount))
        {
            error = "initialScrollIndex is outside the virtual collection";
            return false;
        }
        if (node.sectionHeaderIndices.size() >
                ViewTreeLimits::MaximumVirtualSectionHeaders ||
            !std::is_sorted(node.sectionHeaderIndices.begin(),
                node.sectionHeaderIndices.end()) ||
            std::adjacent_find(node.sectionHeaderIndices.begin(),
                node.sectionHeaderIndices.end()) !=
                    node.sectionHeaderIndices.end() ||
            std::any_of(node.sectionHeaderIndices.begin(),
                node.sectionHeaderIndices.end(), [&](std::size_t index) {
                    return index == 0 || index > node.itemCount;
                }))
        {
            error = "sectionHeaderIndices must be sorted unique bounded 1-based indices";
            return false;
        }
        if (node.stickyHeaderIndex &&
            !std::binary_search(node.sectionHeaderIndices.begin(),
                node.sectionHeaderIndices.end(), *node.stickyHeaderIndex))
        {
            error = "stickyHeaderIndex must reference sectionHeaderIndices";
            return false;
        }
        const std::size_t windowChildOffset = node.stickyHeaderIndex &&
            *node.stickyHeaderIndex < node.firstIndex &&
            node.collectionContent == ViewCollectionContent::Items ? 1 : 0;
        const std::size_t windowChildCount =
            node.children.size() >= windowChildOffset
            ? node.children.size() - windowChildOffset : 0;
        if (windowChildCount > ViewTreeLimits::MaximumVirtualWindowItems ||
            node.children.size() >
                ViewTreeLimits::MaximumVirtualWindowItems + 1)
        {
            error = "virtual collection materialized window exceeds 128 items";
            return false;
        }
        ViewVirtualRange range;
        if (variable)
        {
            if (!ComputeViewVariableVirtualRange(node.itemCount,
                    *node.estimatedItemSize,
                    VirtualCollectionMainGap(node), 1.0f, 0.0f,
                    node.overscan, node.virtualMeasurements,
                    range, error))
                return false;
        }
        else if (!ComputeViewVirtualRange(node.itemCount, node.itemExtent,
                node.type == ViewNodeType::VirtualGrid ? node.columns : 1,
                VirtualCollectionMainGap(node), 1.0f, 0.0f,
                node.overscan, range, error))
            return false;
        if (node.collectionContent != ViewCollectionContent::Items)
        {
            if (node.collectionContent == ViewCollectionContent::Empty &&
                node.itemCount != 0)
            {
                error = "virtual emptyContent requires itemCount 0";
                return false;
            }
        }
        else if (node.itemCount == 0)
        {
            if (node.firstIndex != 0 || !node.children.empty() ||
                !node.sectionHeaderIndices.empty() || node.stickyHeaderIndex)
            {
                error = "empty virtual collections require firstIndex 0 and no children";
                return false;
            }
        }
        else
        {
            if (node.firstIndex == 0 || node.firstIndex > node.itemCount ||
                windowChildCount == 0 ||
                windowChildCount >
                    node.itemCount - (node.firstIndex - 1) ||
                node.virtualChildIndices.size() != node.children.size() ||
                !std::all_of(node.children.begin(), node.children.end(),
                    [](const ViewNode& child) { return child.visible; }))
            {
                error = "virtual collection window must be a non-empty visible contiguous item range";
                return false;
            }
            if (windowChildOffset != 0 &&
                (node.virtualChildIndices.front() !=
                    *node.stickyHeaderIndex ||
                 node.children.front().key.empty()))
            {
                error = "virtual sticky header child does not match stickyHeaderIndex";
                return false;
            }
            for (std::size_t child = windowChildOffset;
                child < node.virtualChildIndices.size(); ++child)
            {
                if (node.virtualChildIndices[child] !=
                    node.firstIndex + child - windowChildOffset)
                {
                    error = "virtual collection window indices must remain contiguous";
                    return false;
                }
            }
            if (node.stickyHeaderIndex &&
                *node.stickyHeaderIndex >= node.firstIndex &&
                *node.stickyHeaderIndex >=
                    node.firstIndex + windowChildCount)
            {
                error = "stickyHeaderIndex must be materialized in or before the visible window";
                return false;
            }
        }
    }
    if (node.type == ViewNodeType::ListItem)
    {
        if (!parentType || !IsCollectionContainer(*parentType))
        {
            error = "listItem nodes must be direct children of a collection";
            return false;
        }
        if (node.sticky && *parentType != ViewNodeType::List)
        {
            error = "sticky listItem nodes require a vertical eager list";
            return false;
        }
        if (++collectionItems > ViewTreeLimits::MaximumCollectionItems)
        {
            error = "view collection item limit exceeded (256)";
            return false;
        }
        if (node.children.size() != 1 || !node.children.front().visible)
        {
            error = "listItem nodes require exactly one child and it must be visible";
            return false;
        }
        if (!accessibleNameDeclared)
        {
            error = "listItem nodes require accessibility.label or accessibility.labelledBy";
            return false;
        }
    }
    if (node.type == ViewNodeType::SlotSurface)
    {
        if (node.logicalSlotId.empty() || node.logicalSlotId.size() > 64)
        {
            error = "slotSurface requires a bounded logical slot id";
            return false;
        }
        if (node.collectionContent == ViewCollectionContent::Empty &&
            (node.children.size() != 1 ||
                !node.children.front().visible ||
                node.children.front().type == ViewNodeType::SlotItem))
        {
            error = "slotSurface emptyContent must be one visible non-slotItem node";
            return false;
        }
        if (node.logicalSlotKind == LogicalSlotKind::Binding)
        {
            if (node.children.size() > 1)
            {
                error = "binding slotSurface accepts at most one child";
                return false;
            }
        }
        else if (node.collectionContent != ViewCollectionContent::Empty &&
            !std::all_of(node.children.begin(), node.children.end(),
                [](const ViewNode& child) {
                    return child.type == ViewNodeType::SlotItem;
                }))
        {
            error = "collection slotSurface children must all be slotItem nodes";
            return false;
        }
    }
    else if (!node.logicalSlotId.empty() || node.logicalSlotRevision != 0)
    {
        error = "logical slot surface state is reserved for slotSurface nodes";
        return false;
    }
    if (node.type == ViewNodeType::SlotItem)
    {
        if (!parentType || *parentType != ViewNodeType::SlotSurface)
        {
            error = "slotItem nodes must be direct children of slotSurface";
            return false;
        }
        if (node.logicalSlotReference.empty() ||
            node.logicalSlotReference.size() > 128)
        {
            error = "slotItem requires a bounded opaque reference";
            return false;
        }
        if (node.children.size() != 1 || !node.children.front().visible)
        {
            error = "slotItem nodes require exactly one visible child";
            return false;
        }
        if (!accessibleNameDeclared)
        {
            error = "slotItem nodes require accessibility.label or accessibility.labelledBy";
            return false;
        }
        if (++collectionItems > ViewTreeLimits::MaximumCollectionItems)
        {
            error = "view collection item limit exceeded (256)";
            return false;
        }
    }
    else if (!node.logicalSlotReference.empty())
    {
        error = "logical slot references are reserved for slotItem nodes";
        return false;
    }
    if (node.type == ViewNodeType::ReferenceIcon)
    {
        if (node.itemReference.empty() || node.itemReference.size() > 128)
        {
            error = "referenceIcon requires a bounded opaque reference";
            return false;
        }
    }
    else if (!node.itemReference.empty())
    {
        error = "item references are reserved for referenceIcon nodes";
        return false;
    }
    if (node.text.size() > ViewTreeLimits::MaximumTextBytes ||
        node.inputValue.size() > ViewTreeLimits::MaximumTextBytes ||
        node.placeholder.size() > ViewTreeLimits::MaximumTextBytes ||
        node.alt.size() > ViewTreeLimits::MaximumTextBytes ||
        node.tooltipTitle.size() >
            ViewTreeLimits::MaximumTooltipTitleBytes ||
        node.tooltip.size() > ViewTreeLimits::MaximumTextBytes ||
        node.validationMessage.size() > ViewTreeLimits::MaximumTextBytes ||
        textBytes + node.debugName.size() + node.testId.size() +
            node.text.size() + node.inputValue.size() +
            node.placeholder.size() + node.alt.size() +
            node.tooltipTitle.size() + node.tooltip.size() +
            node.validationMessage.size() + node.locale.size() >
            ViewTreeLimits::MaximumTotalTextBytes)
    {
        error = "view tree text limit exceeded";
        return false;
    }
    textBytes += node.debugName.size() + node.testId.size() +
        node.text.size() + node.inputValue.size() +
        node.placeholder.size() + node.alt.size() +
        node.tooltipTitle.size() + node.tooltip.size() +
        node.validationMessage.size() + node.locale.size();
    if (node.type == ViewNodeType::StyledText)
    {
        if (node.spans.empty() ||
            node.spans.size() > ViewTreeLimits::MaximumTextSpans)
        {
            error = "styledText spans must contain 1 to 64 items";
            return false;
        }
        std::string combined;
        combined.reserve(node.text.size());
        std::unordered_set<std::string> spanKeys;
        for (const auto& span : node.spans)
        {
            if (span.text.empty() ||
                (span.fontSize &&
                    !FiniteInRange(*span.fontSize, 1.0f, 512.0f)))
            {
                error = "styledText spans require non-empty bounded text and font sizes";
                return false;
            }
            if (span.icon && !IsSingleUtf8Scalar(span.text))
            {
                error = "styledText icon spans require one valid UTF-8 scalar glyph";
                return false;
            }
            if (span.icon && (span.bold || span.italic))
            {
                error = "styledText icon spans do not accept bold or italic";
                return false;
            }
            const bool hasInteractionMetadata = !span.events.empty() ||
                !span.cursor.empty() || !span.tooltipTitle.empty() ||
                !span.tooltip.empty() ||
                !span.accessibilityLabel.empty() ||
                span.hoverForeground.has_value() ||
                span.pressedForeground.has_value();
            if (hasInteractionMetadata && span.key.empty())
            {
                error = "interactive styledText spans require a stable key";
                return false;
            }
            if (!span.key.empty())
            {
                if (span.icon && span.accessibilityLabel.empty())
                {
                    error = "keyed styledText icon spans require accessibility.label";
                    return false;
                }
                const std::string regionKey = node.key + "/" + span.key;
                if (span.key.size() > 128 || regionKey.size() > 128 ||
                    !spanKeys.insert(span.key).second)
                {
                    error = "styledText span keys must be unique and bounded";
                    return false;
                }
                if (!keys.insert(regionKey).second)
                {
                    error = "duplicate generated styledText span key: " +
                        regionKey;
                    return false;
                }
                const std::size_t extraBytes = span.key.size() +
                    span.tooltipTitle.size() + span.tooltip.size() +
                    span.accessibilityLabel.size();
                if (span.tooltipTitle.size() >
                    ViewTreeLimits::MaximumTooltipTitleBytes ||
                    span.tooltip.size() > ViewTreeLimits::MaximumTextBytes)
                {
                    error = "styledText tooltip title/body exceeds its limit";
                    return false;
                }
                if (extraBytes >
                    ViewTreeLimits::MaximumTotalTextBytes - textBytes)
                {
                    error = "view tree text limit exceeded";
                    return false;
                }
                textBytes += extraBytes;
            }
            combined += span.text;
        }
        if (combined != node.text)
        {
            error = "styledText flattened text does not match its spans";
            return false;
        }
    }
    else if (!node.spans.empty())
    {
        error = "text spans are reserved for styledText nodes";
        return false;
    }
    if (node.type == ViewNodeType::MonthCalendar)
    {
        if (node.key.size() > 117 ||
            node.calendarYear < 1 || node.calendarYear > 9999 ||
            node.calendarMonth < 1 || node.calendarMonth > 12 ||
            node.firstDayOfWeek < 1 || node.firstDayOfWeek > 7 ||
            !accessibleNameDeclared)
        {
            error = "monthCalendar requires a bounded key, year/month, firstDayOfWeek, and accessibility.label or accessibility.labelledBy";
            return false;
        }
        std::array<ViewMonthCalendarCell, 42> cells;
        if (!BuildViewMonthCalendarCells(node, cells, error)) return false;
        std::unordered_set<std::string> generatedDates;
        std::size_t calendarTextBytes = node.calendarSelectedDate.size() +
            node.calendarTodayDate.size();
        for (const auto& label : node.weekdayLabels)
        {
            if (label.empty() ||
                label.size() > ViewTreeLimits::MaximumTextBytes)
            {
                error = "monthCalendar weekdayLabels must be seven non-empty bounded strings";
                return false;
            }
            calendarTextBytes += label.size();
        }
        for (const auto& date : node.calendarEventDates)
        {
            if (!generatedDates.insert(date).second)
            {
                error = "monthCalendar eventDates must be unique";
                return false;
            }
            calendarTextBytes += date.size();
        }
        if (calendarTextBytes >
            ViewTreeLimits::MaximumTotalTextBytes - textBytes)
        {
            error = "view tree calendar text limit exceeded";
            return false;
        }
        textBytes += calendarTextBytes;
        for (const auto& cell : cells)
        {
            const std::string generatedKey = node.key + "/" + cell.date;
            if (!keys.insert(generatedKey).second)
            {
                error = "duplicate generated monthCalendar key: " +
                    generatedKey;
                return false;
            }
        }
    }
    else if (node.calendarYear != 0 || node.calendarMonth != 0 ||
        node.firstDayOfWeek != 1 ||
        !node.calendarSelectedDate.empty() ||
        !node.calendarTodayDate.empty() ||
        !node.calendarEventDates.empty() ||
        std::any_of(node.weekdayLabels.begin(), node.weekdayLabels.end(),
            [](const std::string& value) { return !value.empty(); }) ||
        !node.showAdjacentDates)
    {
        error = "calendar state is reserved for monthCalendar nodes";
        return false;
    }
    if (IsInputNode(node.type) &&
        (node.maximumUtf8Bytes > 64 * 1024 ||
            (node.maximumUtf8Bytes > 0 &&
                node.inputValue.size() > node.maximumUtf8Bytes)))
    {
        error = "input maxBytes must be at most 65536 and contain the controlled value";
        return false;
    }
    const std::size_t accessibilityTextBytes =
        node.accessibilityLabel.size() +
        node.accessibilityValue.size() +
        node.accessibilityHint.size() +
        node.accessibilityLabelledBy.size() +
        node.accessibilityDescribedBy.size();
    const bool indexedGridItem = node.accessibilityRowIndex != 0 ||
        node.accessibilityColumnIndex != 0;
    if (node.accessibilityRole.size() > 128 ||
        node.accessibilityLabel.size() > ViewTreeLimits::MaximumTextBytes ||
        node.accessibilityValue.size() > ViewTreeLimits::MaximumTextBytes ||
        node.accessibilityHint.size() > ViewTreeLimits::MaximumTextBytes ||
        node.accessibilityLabelledBy.size() > 128 ||
        node.accessibilityDescribedBy.size() > 128 ||
        accessibilityTextBytes >
            ViewTreeLimits::MaximumTotalTextBytes - textBytes ||
        node.accessibilityHeadingLevel < 0 ||
        node.accessibilityHeadingLevel > 9 ||
        node.accessibilityPositionInSet < 0 ||
        node.accessibilityPositionInSet > 32767 ||
        node.accessibilitySetSize < 0 ||
        node.accessibilitySetSize > 32767 ||
        (node.accessibilityPositionInSet != 0 &&
            node.accessibilitySetSize != 0 &&
            node.accessibilityPositionInSet > node.accessibilitySetSize) ||
        node.accessibilityRowIndex < 0 ||
        node.accessibilityRowIndex > 32767 ||
        node.accessibilityColumnIndex < 0 ||
        node.accessibilityColumnIndex > 32767 ||
        (indexedGridItem &&
            (node.accessibilityRowIndex == 0 ||
                node.accessibilityColumnIndex == 0 || !parentType ||
                !IsGridContainer(*parentType))))
    {
        error = "view accessibility metadata is invalid or exceeds its limits";
        return false;
    }
    textBytes += accessibilityTextBytes;
    if (IsChoiceNode(node.type))
    {
        if (node.options.empty() ||
            node.options.size() > ViewTreeLimits::MaximumChoiceOptions)
        {
            error = "choice options must contain 1 to 64 items";
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
                error = "choice option keys, values, and labels must be non-empty and bounded";
                return false;
            }
            if (!optionKeys.insert(option.key).second ||
                !optionValues.insert(option.value).second)
            {
                error = "choice option keys and values must be unique";
                return false;
            }
            if (!keys.insert(regionKey).second)
            {
                error = "duplicate generated choice option key: " + regionKey;
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
            error = "choice selectedValue must match an option or be empty";
            return false;
        }
    }
    else if (!node.options.empty() || !node.selectedValue.empty())
    {
        error = "choice options are reserved for radioGroup and select nodes";
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
        if (!accessibleNameDeclared)
        {
            error = std::string(ViewNodeTypeName(node.type)) +
                " nodes require accessibility.label or accessibility.labelledBy";
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
    if ((node.imageTint || node.imageTintToken) &&
        (node.type != ViewNodeType::Image ||
            (node.imageTint && *node.imageTint > 0xFFFFFF)))
    {
        error = "image tint is only valid for image nodes and must be an RGB color";
        return false;
    }
    if (!node.fontResourceName.empty() &&
        node.type != ViewNodeType::Text &&
        node.type != ViewNodeType::StyledText &&
        node.type != ViewNodeType::Button &&
        node.type != ViewNodeType::Link &&
        node.type != ViewNodeType::Badge &&
        !IsCheckControlNode(node.type) &&
        node.type != ViewNodeType::RadioGroup &&
        node.type != ViewNodeType::MonthCalendar)
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
    if (node.type == ViewNodeType::StyledText && node.text.empty())
    {
        error = "styledText nodes require non-empty spans";
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
        !accessibleNameDeclared)
    {
        error = "iconButton nodes require accessibility.label or accessibility.labelledBy";
        return false;
    }
    if (node.type == ViewNodeType::Meter &&
        !accessibleNameDeclared)
    {
        error = "meter nodes require accessibility.label or accessibility.labelledBy";
        return false;
    }
    if (node.type == ViewNodeType::Slider &&
        !accessibleNameDeclared)
    {
        error = "slider nodes require accessibility.label or accessibility.labelledBy";
        return false;
    }
    if ((IsInputNode(node.type) || node.type == ViewNodeType::Select) &&
        !accessibleNameDeclared)
    {
        error = std::string(ViewNodeTypeName(node.type)) +
            " nodes require accessibility.label or accessibility.labelledBy";
        return false;
    }
    for (const auto& [eventName, action] : node.events)
    {
        if (!IsKnownViewEvent(eventName))
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
    if (!ViewNodeAllowsEvent(node.type, "focus") &&
        (node.events.contains("focus") || node.events.contains("blur") ||
            node.events.contains("submit")))
    {
        error = "focus, blur, and submit are reserved for input nodes";
        return false;
    }
    const bool textSelectionInput =
        ViewNodeAllowsEvent(node.type, "selectionChange");
    if ((node.textSelection.has_value() ||
            node.events.contains("selectionChange")) &&
        (!textSelectionInput || !node.textSelection ||
            !node.events.contains("selectionChange")))
    {
        error = "controlled text selection requires selection and selectionChange on a text input";
        return false;
    }
    if ((node.events.contains("keyDown") ||
            node.events.contains("keyUp")) && !keyboardFocusable)
    {
        error = "view keyDown and keyUp events require a focusable node";
        return false;
    }
    if (!ViewNodeAllowsEvent(node.type, "scrollEnd") &&
        node.events.contains("scrollEnd"))
    {
        error = "scrollEnd events are reserved for scroll and virtual collection nodes";
        return false;
    }
    if (node.capturePointer &&
        !node.events.contains("pointerMove") &&
        !node.events.contains("pointerUp"))
    {
        error = "view capturePointer requires pointerMove or pointerUp";
        return false;
    }
    if (node.type == ViewNodeType::Select)
    {
        if (!node.events.contains("change") ||
            !node.events.contains("click"))
        {
            error = "select nodes require click and change";
            return false;
        }
    }
    else if (IsCollectionContainer(node.type) &&
        node.selectionMode != ViewSelectionMode::None)
    {
        if (!node.events.contains("change") ||
            node.events.contains("click"))
        {
            error = "selectable collection nodes require change and reject click";
            return false;
        }
    }
    else if (IsControlledNode(node.type))
    {
        const bool changeRequired =
            !IsInputNode(node.type) || !node.readOnly;
        if ((changeRequired && !node.events.contains("change")) ||
            node.events.contains("click"))
        {
            error = std::string(ViewNodeTypeName(node.type)) +
                (changeRequired
                    ? " nodes require change and reject click"
                    : " nodes reject click");
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
                seriesPoints, collectionItems, keys, accessKeys, resources,
                node.type, error)) return false;
    return true;
}

const ViewNode* FindAccessibilityReferenceTarget(
    const ViewNode& node, std::string_view key) noexcept
{
    if (node.key == key) return &node;
    for (const auto& child : node.children)
        if (const auto* target =
                FindAccessibilityReferenceTarget(child, key))
            return target;
    return nullptr;
}

std::string_view AccessibilityReferenceText(const ViewNode& node) noexcept
{
    if (!node.accessibilityLabel.empty()) return node.accessibilityLabel;
    if (!node.alt.empty()) return node.alt;
    return node.text;
}

bool AccessibilitySubtreeContainsInteraction(const ViewNode& node) noexcept
{
    const auto* contract = FindViewNodeContract(node.type);
    if ((contract && contract->keyboardFocusable) ||
        node.focusable.value_or(false) || !node.events.empty())
        return true;
    return std::any_of(node.children.begin(), node.children.end(),
        [](const ViewNode& child) {
            return AccessibilitySubtreeContainsInteraction(child);
        });
}
}

bool ValidateViewTreeStructure(const ViewNode& root, std::string& error)
{
    std::size_t nodes = 0;
    std::size_t textBytes = 0;
    std::size_t seriesPoints = 0;
    std::size_t collectionItems = 0;
    std::unordered_set<std::string> keys;
    std::unordered_set<char> accessKeys;
    std::unordered_set<std::string> resources;
    return ValidateNode(root, 0, nodes, textBytes, seriesPoints,
        collectionItems, keys, accessKeys, resources, std::nullopt, error);
}

bool ValidateAccessibilityMetadata(const ViewNode& root,
    std::string& error)
{
    const auto validate = [&](const auto& self,
        const ViewNode& node) -> bool {
        if (node.accessibilityHidden &&
            AccessibilitySubtreeContainsInteraction(node))
        {
            error = "accessibility.hidden cannot conceal an interactive subtree";
            return false;
        }
        for (const auto* reference : {
                &node.accessibilityLabelledBy,
                &node.accessibilityDescribedBy })
        {
            if (reference->empty()) continue;
            const auto* target = FindAccessibilityReferenceTarget(
                root, *reference);
            if (!target || target == &node ||
                AccessibilityReferenceText(*target).empty())
            {
                error = "view accessibility relationship references an invalid or unnamed node";
                return false;
            }
        }
        if (IsGridContainer(node.type))
        {
            std::unordered_set<std::uint64_t> explicitCells;
            for (const auto& child : node.children)
            {
                if (child.accessibilityRowIndex == 0) continue;
                const std::uint64_t identity =
                    (static_cast<std::uint64_t>(
                        child.accessibilityRowIndex) << 32) |
                    static_cast<std::uint32_t>(
                        child.accessibilityColumnIndex);
                if (!explicitCells.insert(identity).second)
                {
                    error = "grid accessibility row/column indices must be unique";
                    return false;
                }
            }
        }
        for (const auto& child : node.children)
            if (!self(self, child)) return false;
        return true;
    };
    return validate(validate, root);
}
}
