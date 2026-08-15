#include "widget_view_tree.h"
#include "widget_view_contract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr float MaximumDimension = 100000.0f;
constexpr float MaximumScrollExtent = 1000000.0f;

struct CivilDate
{
    int year = 0;
    int month = 0;
    int day = 0;
    int weekday = 0;
};

int DaysInMonth(int year, int month) noexcept
{
    static constexpr std::array<int, 12> days = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) return 0;
    if (month != 2) return days[static_cast<std::size_t>(month - 1)];
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
        ? 29 : 28;
}

long long DaysFromCivil(int year, unsigned month, unsigned day) noexcept
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 + doe - 719468;
}

CivilDate CivilFromDays(long long days) noexcept
{
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const int month = static_cast<int>(mp) + (mp < 10 ? 3 : -9);
    year += month <= 2;
    int weekday = static_cast<int>((days - 719468 + 4) % 7);
    if (weekday < 0) weekday += 7;
    return { year, month, static_cast<int>(day), weekday + 1 };
}

bool ParseCivilDate(std::string_view value, CivilDate& date) noexcept
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        return false;
    int parts[3]{};
    const std::array<std::pair<std::size_t, std::size_t>, 3> ranges = {
        std::pair<std::size_t, std::size_t>{ 0, 4 }, { 5, 2 }, { 8, 2 }
    };
    for (std::size_t part = 0; part < ranges.size(); ++part)
    {
        const auto [offset, length] = ranges[part];
        for (std::size_t index = 0; index < length; ++index)
        {
            const char ch = value[offset + index];
            if (ch < '0' || ch > '9') return false;
            parts[part] = parts[part] * 10 + (ch - '0');
        }
    }
    if (parts[0] < 1 || parts[0] > 9999 || parts[1] < 1 ||
        parts[1] > 12 || parts[2] < 1 ||
        parts[2] > DaysInMonth(parts[0], parts[1]))
        return false;
    const long long serial = DaysFromCivil(parts[0],
        static_cast<unsigned>(parts[1]), static_cast<unsigned>(parts[2]));
    int weekday = static_cast<int>((serial + 4) % 7);
    if (weekday < 0) weekday += 7;
    date = { parts[0], parts[1], parts[2], weekday + 1 };
    return true;
}

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

std::string FormatCivilDate(const CivilDate& date)
{
    char buffer[11]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
        date.year, date.month, date.day);
    return buffer;
}

bool FiniteInRange(float value, float minimum, float maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool ValidateLength(const ViewLength& length) noexcept
{
    return length.kind != ViewLengthKind::Fixed ||
        FiniteInRange(length.value, 0.0f, MaximumDimension);
}

float ConstrainDimension(float value, const std::optional<float>& minimum,
    const std::optional<float>& maximum) noexcept
{
    if (minimum) value = std::max(value, *minimum);
    if (maximum) value = std::min(value, *maximum);
    return std::max(0.0f, value);
}

float ConstrainWidth(const ViewNode& node, float value) noexcept
{
    return ConstrainDimension(
        value, node.minimumWidth, node.maximumWidth);
}

float ConstrainHeight(const ViewNode& node, float value) noexcept
{
    return ConstrainDimension(
        value, node.minimumHeight, node.maximumHeight);
}

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

float TextIntrinsicLineHeight(const ViewNode& node) noexcept
{
    return node.lineHeight.value_or(node.fontSize * 1.4f);
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

bool IsInputNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::TextInput ||
        type == ViewNodeType::TextArea ||
        type == ViewNodeType::SearchBox ||
        type == ViewNodeType::NumberInput;
}

bool IsChoiceNode(ViewNodeType type) noexcept
{
    return type == ViewNodeType::RadioGroup ||
        type == ViewNodeType::Select;
}

bool IsControlledNode(ViewNodeType type) noexcept
{
    return IsCheckControlNode(type) ||
        IsChoiceNode(type) || IsInputNode(type) ||
        type == ViewNodeType::Slider ||
        type == ViewNodeType::MonthCalendar;
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
    return type == ViewNodeType::Text ||
        type == ViewNodeType::StyledText ||
        type == ViewNodeType::Image ||
        type == ViewNodeType::ReferenceIcon ||
        IsButtonNode(type) || type == ViewNodeType::Link ||
        IsControlledNode(type) ||
        type == ViewNodeType::Icon || type == ViewNodeType::Shape ||
        type == ViewNodeType::Badge || type == ViewNodeType::Divider ||
        type == ViewNodeType::ProgressBar ||
        type == ViewNodeType::ProgressRing ||
        type == ViewNodeType::Meter ||
        IsDataSeriesNode(type) || type == ViewNodeType::MonthCalendar ||
        type == ViewNodeType::Spacer;
}

bool IsGridContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Grid || type == ViewNodeType::GridList ||
        type == ViewNodeType::VirtualGrid;
}

bool IsPositionedGridContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Grid || type == ViewNodeType::GridList;
}

bool IsFlexContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Row || type == ViewNodeType::Column;
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

bool IsVirtualCollection(ViewNodeType type) noexcept
{
    return type == ViewNodeType::VirtualList ||
        type == ViewNodeType::VirtualGrid;
}

bool IsCollectionContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::List || type == ViewNodeType::GridList ||
        IsVirtualCollection(type);
}

bool HasCollectionPlaceholder(const ViewNode& node) noexcept
{
    return IsCollectionContainer(node.type) &&
        node.collectionContent != ViewCollectionContent::Items;
}

bool IsScrollContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Scroll || IsVirtualCollection(type);
}

const char* DefaultAccessibilityRole(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract ? contract->defaultAccessibilityRole.data() : "";
}

bool IsNodeKeyboardFocusable(const ViewNode& node) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(node.type);
    return node.focusable.value_or(
        contract && contract->keyboardFocusable);
}

void ApplyNodeFocusPolicy(const ViewNode& node,
    InteractionRegion& region) noexcept
{
    region.focusable = IsNodeKeyboardFocusable(node);
    region.tabIndex = node.tabIndex.value_or(0);
}

ViewRect ContentRect(const ViewNode& node) noexcept
{
    const ViewEdgeInsets insets = ResolveInsets(
        node.padding, node.frame.width, node.frame.height);
    return { node.frame.x + insets.left, node.frame.y + insets.top,
        std::max(0.0f, node.frame.width - HorizontalInsets(insets)),
        std::max(0.0f, node.frame.height - VerticalInsets(insets)) };
}

std::optional<ViewRect> IntersectRects(
    const std::optional<ViewRect>& first, const ViewRect& second) noexcept
{
    if (!first) return second;
    const float left = std::max(first->x, second.x);
    const float top = std::max(first->y, second.y);
    const float right = std::min(first->x + first->width,
        second.x + second.width);
    const float bottom = std::min(first->y + first->height,
        second.y + second.height);
    if (right <= left || bottom <= top) return std::nullopt;
    return ViewRect{ left, top, right - left, bottom - top };
}

bool Overlaps(const ViewRect& rect, const ViewRect& clip) noexcept
{
    return rect.x < clip.x + clip.width &&
        rect.x + rect.width > clip.x &&
        rect.y < clip.y + clip.height &&
        rect.y + rect.height > clip.y;
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
    if (node.type == ViewNodeType::Text ||
        node.type == ViewNodeType::StyledText ||
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
    const std::size_t columns = node.type == ViewNodeType::VirtualGrid
        ? node.columns : 1;
    if (columns == 0 || node.firstIndex == 0) return;
    const float columnGap = node.columnGap.value_or(node.gap);
    const float rowGap = node.rowGap.value_or(node.gap);
    const float cellWidth = std::max(0.0f,
        (content.width - columnGap * static_cast<float>(columns - 1)) /
            static_cast<float>(columns));
    for (std::size_t childIndex = 0;
        childIndex < node.children.size(); ++childIndex)
    {
        ViewNode& child = node.children[childIndex];
        if (!child.visible) continue;
        const std::size_t itemIndex = node.firstIndex + childIndex;
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
    node.scrollOffset = 0.0f;
    node.scrollViewportExtent = 0.0f;
    node.scrollContentExtent = 0.0f;
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

bool ValidateNode(const ViewNode& node, std::size_t depth,
    std::size_t& nodes, std::size_t& textBytes,
    std::size_t& seriesPoints, std::size_t& collectionItems,
    std::unordered_set<std::string>& keys,
    std::unordered_set<std::string>& resources,
    std::optional<ViewNodeType> parentType, std::string& error)
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
    if (node.transition)
    {
        if (node.transition->durationMilliseconds < 1 ||
            node.transition->durationMilliseconds > 2000 ||
            node.transition->properties.empty() ||
            node.transition->properties.size() > 4)
        {
            error = "view transition must contain 1 to 4 properties and a duration from 1 to 2000ms";
            return false;
        }
        std::array<bool, 4> seen{};
        for (const auto property : node.transition->properties)
        {
            const auto index = static_cast<std::size_t>(property);
            if (index >= seen.size() || seen[index])
            {
                error = "view transition properties must be supported and unique";
                return false;
            }
            seen[index] = true;
        }
        switch (node.transition->easing)
        {
        case ViewTransitionEasing::Linear:
        case ViewTransitionEasing::EaseIn:
        case ViewTransitionEasing::EaseOut:
        case ViewTransitionEasing::EaseInOut:
            break;
        default:
            error = "view transition easing is unsupported";
            return false;
        }
    }
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
    if (node.indeterminate &&
        (node.type != ViewNodeType::Checkbox || node.checked))
    {
        error = "view indeterminate requires a checkbox with checked=false";
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
        node.alt.empty())
    {
        error = "view focusable=true requires accessible text or accessibility.label";
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
    if (node.type == ViewNodeType::VirtualList && node.columnGap)
    {
        error = "columnGap is reserved for grid nodes";
        return false;
    }
    if (!IsVirtualCollection(node.type) &&
        (node.itemCount != 0 || node.firstIndex != 0 ||
            node.itemExtent != 0.0f || node.overscan != 2))
    {
        error = "virtual collection metadata is reserved for virtual nodes";
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
    }
    else if (node.selectionMode != ViewSelectionMode::None ||
        !node.selectedKeys.empty() ||
        node.collectionContent != ViewCollectionContent::Items)
    {
        error = "selectionMode and selectedKeys are reserved for collection nodes";
        return false;
    }
    if (IsVirtualCollection(node.type))
    {
        if (node.orientation != ViewOrientation::Vertical)
        {
            error = "virtual collection nodes are vertical";
            return false;
        }
        if (node.height.kind == ViewLengthKind::Auto)
        {
            error = "virtual collection nodes require fixed or fill height";
            return false;
        }
        if (node.itemCount > ViewTreeLimits::MaximumVirtualItemCount ||
            node.overscan > ViewTreeLimits::MaximumVirtualOverscan)
        {
            error = "virtual collection itemCount or overscan exceeds its limit";
            return false;
        }
        if (node.children.size() >
            ViewTreeLimits::MaximumVirtualWindowItems)
        {
            error = "virtual collection materialized window exceeds 128 items";
            return false;
        }
        ViewVirtualRange range;
        if (!ComputeViewVirtualRange(node.itemCount, node.itemExtent,
                node.type == ViewNodeType::VirtualGrid ? node.columns : 1,
                node.rowGap.value_or(node.gap), 1.0f, 0.0f,
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
            if (node.firstIndex != 0 || !node.children.empty())
            {
                error = "empty virtual collections require firstIndex 0 and no children";
                return false;
            }
        }
        else
        {
            if (node.firstIndex == 0 || node.firstIndex > node.itemCount ||
                node.children.empty() ||
                node.children.size() >
                    node.itemCount - (node.firstIndex - 1) ||
                !std::all_of(node.children.begin(), node.children.end(),
                    [](const ViewNode& child) { return child.visible; }))
            {
                error = "virtual collection window must be a non-empty visible contiguous item range";
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
        if (node.accessibilityLabel.empty())
        {
            error = "listItem nodes require accessibility.label";
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
        if (node.logicalSlotKind == LogicalSlotKind::Binding)
        {
            if (node.children.size() > 1)
            {
                error = "binding slotSurface accepts at most one child";
                return false;
            }
        }
        else if (!std::all_of(node.children.begin(), node.children.end(),
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
        if (node.accessibilityLabel.empty())
        {
            error = "slotItem nodes require accessibility.label";
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
        node.tooltip.size() > ViewTreeLimits::MaximumTextBytes ||
        node.validationMessage.size() > ViewTreeLimits::MaximumTextBytes ||
        textBytes + node.text.size() + node.inputValue.size() +
            node.placeholder.size() + node.alt.size() + node.tooltip.size() +
            node.validationMessage.size() + node.locale.size() >
            ViewTreeLimits::MaximumTotalTextBytes)
    {
        error = "view tree text limit exceeded";
        return false;
    }
    textBytes += node.text.size() + node.inputValue.size() +
        node.placeholder.size() + node.alt.size() + node.tooltip.size() +
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
            const bool hasInteractionMetadata = !span.events.empty() ||
                !span.cursor.empty() || !span.tooltip.empty() ||
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
                    span.tooltip.size() + span.accessibilityLabel.size();
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
            node.accessibilityLabel.empty())
        {
            error = "monthCalendar requires a bounded key, year/month, firstDayOfWeek, and accessibility.label";
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
    if (node.imageTint && (node.type != ViewNodeType::Image ||
            *node.imageTint > 0xFFFFFF))
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
    if ((IsInputNode(node.type) || node.type == ViewNodeType::Select) &&
        node.accessibilityLabel.empty())
    {
        error = std::string(ViewNodeTypeName(node.type)) +
            " nodes require accessibility.label";
        return false;
    }
    for (const auto& [eventName, action] : node.events)
    {
        if (eventName != "click" && eventName != "doubleClick" &&
            eventName != "contextMenu" && eventName != "pointerEnter" &&
            eventName != "pointerLeave" && eventName != "pointerDown" &&
            eventName != "pointerUp" && eventName != "keyDown" &&
            eventName != "keyUp" && eventName != "change" &&
            eventName != "selectionChange" && eventName != "focus" &&
            eventName != "blur" &&
            eventName != "submit" && eventName != "scrollEnd")
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
    if (!IsInputNode(node.type) &&
        (node.events.contains("focus") || node.events.contains("blur") ||
            node.events.contains("submit")))
    {
        error = "focus, blur, and submit are reserved for input nodes";
        return false;
    }
    const bool textSelectionInput = node.type == ViewNodeType::TextInput ||
        node.type == ViewNodeType::TextArea ||
        node.type == ViewNodeType::SearchBox;
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
    if (!IsScrollContainer(node.type) && node.events.contains("scrollEnd"))
    {
        error = "scrollEnd events are reserved for scroll and virtual collection nodes";
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
                seriesPoints, collectionItems, keys, resources,
                node.type, error)) return false;
    return true;
}

void ApplyCollectionSelectionState(ViewNode& node)
{
    if (IsCollectionContainer(node.type) &&
        node.collectionContent == ViewCollectionContent::Items &&
        node.selectionMode != ViewSelectionMode::None)
    {
        const auto change = node.events.find("change");
        for (auto& child : node.children)
        {
            child.inheritedSelectionMode = node.selectionMode;
            child.inheritedSelectedKeys = node.selectedKeys;
            child.selected = std::find(node.selectedKeys.begin(),
                node.selectedKeys.end(), child.key) !=
                node.selectedKeys.end();
            if (change != node.events.end())
                child.inheritedSelectionChangeAction = change->second;
        }
    }
    for (auto& child : node.children)
        ApplyCollectionSelectionState(child);
}

void ClearCollectionSelectionState(ViewNode& node)
{
    if (node.inheritedSelectionMode != ViewSelectionMode::None)
        node.selected = false;
    node.inheritedSelectionMode = ViewSelectionMode::None;
    node.inheritedSelectedKeys.clear();
    node.inheritedSelectionChangeAction.reset();
    for (auto& child : node.children)
        ClearCollectionSelectionState(child);
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
            surface.tooltip = node.tooltip;
            surface.accessibilityRole = node.accessibilityRole.empty()
                ? "grid" : node.accessibilityRole;
            surface.accessibilityLabel = node.accessibilityLabel;
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
            region.tooltip = node.tooltip;
            region.events = node.events;
            region.controlKind = InteractionControlKind::Radio;
            region.checked = cell.selected;
            region.currentSelection = node.calendarSelectedDate;
            region.proposedSelection = cell.date;
            region.accessibilityRole = "gridcell";
            region.accessibilityLabel = cell.date;
            region.enabled = node.enabled;
            ApplyNodeFocusPolicy(node, region);
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
            region.tooltip = node.tooltip;
            region.events = node.events;
            region.controlKind = InteractionControlKind::Radio;
            region.checked = option.value == node.selectedValue;
            region.currentSelection = node.selectedValue;
            region.proposedSelection = option.value;
            region.accessibilityRole = "radio";
            region.accessibilityLabel = option.label;
            region.enabled = node.enabled && option.enabled;
            ApplyNodeFocusPolicy(node, region);
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
            const ViewRect content = ContentRect(node);
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
            if (inheritedClip)
                region.clip = InteractionClipRect{ inheritedClip->x,
                    inheritedClip->y, inheritedClip->width,
                    inheritedClip->height };
            region.cursor = "hand";
            region.tooltip = node.tooltip;
            for (const auto& [name, action] : node.events)
                if (name != "click") region.events.emplace(name, action);
            region.controlKind = InteractionControlKind::Radio;
            region.checked = option.value == node.selectedValue;
            region.currentSelection = node.selectedValue;
            region.proposedSelection = option.value;
            region.accessibilityRole = "option";
            region.accessibilityLabel = option.label;
            region.enabled = node.enabled && option.enabled;
            ApplyNodeFocusPolicy(node, region);
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

void TranslateTree(ViewNode& node, float deltaX, float deltaY) noexcept
{
    node.frame.x += deltaX;
    node.frame.y += deltaY;
    if (node.clipFrame)
    {
        node.clipFrame->x += deltaX;
        node.clipFrame->y += deltaY;
    }
    for (auto& child : node.children)
        TranslateTree(child, deltaX, deltaY);
}

bool ApplyScrollState(ViewNode& node,
    const ViewScrollOffsetResolver& resolver,
    std::vector<ViewScrollViewport>& viewports,
    const std::optional<ViewRect>& inheritedClip,
    std::size_t& scrollContainers, std::string& error)
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return true;
    std::optional<ViewRect> childClip = inheritedClip;
    if (!IsScrollContainer(node.type) && node.clipFrame)
    {
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    }
    if (IsScrollContainer(node.type))
    {
        if (++scrollContainers > ViewTreeLimits::MaximumScrollContainers)
        {
            error = "view scroll container limit exceeded (32)";
            return false;
        }
        if (node.type == ViewNodeType::Scroll &&
            (node.children.size() != 1 || !node.children.front().visible))
        {
            error = "view scroll state requires one visible child";
            return false;
        }
        const ViewRect clip = ContentRect(node);
        if (clip.width <= 0.0f || clip.height <= 0.0f)
        {
            error = "view scroll viewport must have positive content bounds";
            return false;
        }
        node.clipFrame = clip;
        const bool vertical = node.orientation == ViewOrientation::Vertical;
        const float viewportExtent = vertical ? clip.height : clip.width;
        float contentExtent = viewportExtent;
        float maximum = 0.0f;
        ViewVirtualRange virtualRange;
        if (IsVirtualCollection(node.type) &&
            !HasCollectionPlaceholder(node))
        {
            if (!ComputeViewVirtualRange(node.itemCount, node.itemExtent,
                    node.type == ViewNodeType::VirtualGrid
                        ? node.columns : 1,
                    node.rowGap.value_or(node.gap), viewportExtent,
                    0.0f, node.overscan, virtualRange, error))
                return false;
            contentExtent = virtualRange.contentExtent;
            maximum = virtualRange.maximum;
        }
        else
        {
            const ViewNode& child = node.children.front();
            contentExtent = std::max(viewportExtent,
                vertical ? child.frame.y + child.frame.height +
                        child.margin.bottom - clip.y :
                    child.frame.x + child.frame.width +
                        child.margin.right - clip.x);
            if (!FiniteInRange(contentExtent, 0.0f, MaximumScrollExtent))
            {
                error = "view scroll content extent exceeds 1000000";
                return false;
            }
            maximum = std::max(0.0f,
                contentExtent - viewportExtent);
        }
        const float requested = resolver ? resolver(node.key, maximum) : 0.0f;
        if (!std::isfinite(requested))
        {
            error = "view scroll resolver returned a non-finite offset";
            return false;
        }
        const float offset = std::clamp(requested, 0.0f, maximum);
        node.scrollOffset = offset;
        node.scrollViewportExtent = viewportExtent;
        node.scrollContentExtent = contentExtent;
        if (IsVirtualCollection(node.type) &&
            !HasCollectionPlaceholder(node))
        {
            ViewVirtualRange visibleRange;
            if (!ComputeViewVirtualRange(node.itemCount, node.itemExtent,
                    node.type == ViewNodeType::VirtualGrid
                        ? node.columns : 1,
                    node.rowGap.value_or(node.gap), viewportExtent,
                    offset, 0, visibleRange, error))
                return false;
            if (node.itemCount > 0)
            {
                const std::size_t providedLast = node.firstIndex +
                    node.children.size() - 1;
                if (node.firstIndex > visibleRange.firstIndex ||
                    providedLast < visibleRange.lastIndex)
                {
                    error = "virtual collection window does not cover the visible range";
                    return false;
                }
            }
            for (auto& child : node.children)
                TranslateTree(child, 0.0f, -offset);
        }
        else if (!IsVirtualCollection(node.type))
        {
            TranslateTree(node.children.front(),
                vertical ? 0.0f : -offset,
                vertical ? -offset : 0.0f);
        }

        childClip = IntersectRects(inheritedClip, clip);
        const ViewRect visibleFrame = childClip.value_or(clip);
        viewports.push_back({ node.key, visibleFrame, node.orientation,
            viewportExtent, contentExtent, offset, maximum });
    }
    for (auto& child : node.children)
        if (!ApplyScrollState(child, resolver, viewports, childClip,
                scrollContainers, error)) return false;
    return true;
}

ViewResolvedTransform ComposeTransform(
    const ViewResolvedTransform& local,
    const ViewResolvedTransform& parent) noexcept
{
    return {
        local.scale * parent.scale,
        local.translateX * parent.scale + parent.translateX,
        local.translateY * parent.scale + parent.translateY
    };
}

ViewResolvedTransform LocalTransform(const ViewNode& node) noexcept
{
    if (!node.transform) return {};
    const auto& value = *node.transform;
    const float originX = node.frame.x +
        node.frame.width * value.originX;
    const float originY = node.frame.y +
        node.frame.height * value.originY;
    return { value.scale,
        originX * (1.0f - value.scale) + value.translateX,
        originY * (1.0f - value.scale) + value.translateY };
}

struct TransformMatch
{
    ViewResolvedTransform node;
    std::optional<ViewRect> parentClip;
    std::optional<ViewRect> nodeClip;
    std::size_t keyLength = 0;
    bool found = false;
};

void FindTransform(const ViewNode& node, std::string_view key,
    const ViewResolvedTransform& parent,
    const std::optional<ViewRect>& inheritedClip,
    bool inheritedClipActive,
    TransformMatch& match) noexcept
{
    const ViewResolvedTransform current =
        ComposeTransform(LocalTransform(node), parent);
    std::optional<ViewRect> childClip = inheritedClip;
    bool childClipActive = inheritedClipActive;
    if (node.clipFrame)
    {
        const ViewRect transformedClip =
            ApplyViewTransform(*node.clipFrame, current);
        childClip = inheritedClipActive
            ? (inheritedClip
                ? IntersectRects(inheritedClip, transformedClip)
                : std::nullopt)
            : std::optional<ViewRect>(transformedClip);
        childClipActive = true;
    }
    const bool exact = key == node.key;
    const bool generated = key.size() > node.key.size() &&
        key.starts_with(node.key) && key[node.key.size()] == '/';
    if ((exact || generated) && node.key.size() >= match.keyLength)
    {
        match.node = current;
        match.parentClip = inheritedClipActive
            ? std::optional<ViewRect>(
                inheritedClip.value_or(ViewRect{}))
            : std::nullopt;
        match.nodeClip = childClipActive
            ? std::optional<ViewRect>(childClip.value_or(ViewRect{}))
            : std::nullopt;
        match.keyLength = node.key.size();
        match.found = true;
    }
    for (const auto& child : node.children)
        FindTransform(child, key, current, childClip,
            childClipActive, match);
}

bool ValidateTransforms(const ViewNode& node,
    const ViewResolvedTransform& parent, std::string& error)
{
    if (node.transform)
    {
        const auto& value = *node.transform;
        if (!FiniteInRange(value.translateX,
                -MaximumDimension, MaximumDimension) ||
            !FiniteInRange(value.translateY,
                -MaximumDimension, MaximumDimension) ||
            !FiniteInRange(value.scale, 0.05f, 8.0f) ||
            !FiniteInRange(value.originX, 0.0f, 1.0f) ||
            !FiniteInRange(value.originY, 0.0f, 1.0f))
        {
            error = "view transform fields are outside their limits";
            return false;
        }
    }
    const ViewResolvedTransform current =
        ComposeTransform(LocalTransform(node), parent);
    if (!FiniteInRange(current.scale, 1.0f / 64.0f, 64.0f))
    {
        error = "view cumulative transform scale must remain between 1/64 and 64";
        return false;
    }
    const ViewRect transformed{
        node.frame.x * current.scale + current.translateX,
        node.frame.y * current.scale + current.translateY,
        node.frame.width * current.scale,
        node.frame.height * current.scale };
    if (!FiniteInRange(transformed.x,
            -MaximumScrollExtent, MaximumScrollExtent) ||
        !FiniteInRange(transformed.y,
            -MaximumScrollExtent, MaximumScrollExtent) ||
        !FiniteInRange(transformed.width, 0.0f, MaximumScrollExtent) ||
        !FiniteInRange(transformed.height, 0.0f, MaximumScrollExtent))
    {
        error = "view transformed bounds exceed 1000000";
        return false;
    }
    for (const auto& child : node.children)
        if (!ValidateTransforms(child, current, error)) return false;
    return true;
}

bool HasTransitionProperty(const ViewTransition& transition,
    ViewTransitionProperty property) noexcept
{
    return std::find(transition.properties.begin(),
        transition.properties.end(), property) !=
        transition.properties.end();
}

float ResolveTransitionProgress(ViewTransitionEasing easing,
    float progress) noexcept
{
    progress = std::clamp(progress, 0.0f, 1.0f);
    switch (easing)
    {
    case ViewTransitionEasing::EaseIn:
        return progress * progress;
    case ViewTransitionEasing::EaseOut:
        return 1.0f - (1.0f - progress) * (1.0f - progress);
    case ViewTransitionEasing::EaseInOut:
        return progress < 0.5f
            ? 2.0f * progress * progress
            : 1.0f - 2.0f * (1.0f - progress) * (1.0f - progress);
    default:
        return progress;
    }
}

std::uint32_t InterpolateTransitionColor(
    std::uint32_t start, std::uint32_t target, float progress) noexcept
{
    const auto channel = [progress](std::uint32_t from,
        std::uint32_t to) {
        return static_cast<std::uint32_t>(std::lround(
            static_cast<float>(from) +
            (static_cast<float>(to) - static_cast<float>(from)) *
                progress));
    };
    return (channel((start >> 16) & 0xFF, (target >> 16) & 0xFF) << 16) |
        (channel((start >> 8) & 0xFF, (target >> 8) & 0xFF) << 8) |
        channel(start & 0xFF, target & 0xFF);
}

bool HasTransitionDifference(const ViewStyle& start,
    const ViewStyle& target, const ViewTransition& transition) noexcept
{
    const auto changedColor = [](const auto& first, const auto& second) {
        return first && second && first != second;
    };
    return (HasTransitionProperty(transition,
                ViewTransitionProperty::Background) &&
            changedColor(start.background, target.background)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Foreground) &&
            changedColor(start.foreground, target.foreground)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::BorderColor) &&
            changedColor(start.borderColor, target.borderColor)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Opacity) &&
            start.opacity.value_or(1.0f) != target.opacity.value_or(1.0f));
}

ViewStyle InterpolateTransitionStyle(const ViewStyle& start,
    const ViewStyle& target, const ViewTransition& transition,
    float progress) noexcept
{
    ViewStyle result = target;
    progress = ResolveTransitionProgress(transition.easing, progress);
    const auto applyColor = [progress](const auto& from, const auto& to,
        auto& output) {
        if (from && to)
            output = InterpolateTransitionColor(*from, *to, progress);
    };
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Background))
        applyColor(start.background, target.background, result.background);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Foreground))
        applyColor(start.foreground, target.foreground, result.foreground);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::BorderColor))
        applyColor(start.borderColor, target.borderColor, result.borderColor);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Opacity))
    {
        const float from = start.opacity.value_or(1.0f);
        const float to = target.opacity.value_or(1.0f);
        result.opacity = from + (to - from) * progress;
    }
    return result;
}
}

ViewRect ViewNodeContentRect(const ViewNode& node) noexcept
{
    return ContentRect(node);
}

ViewResolvedTransform ResolveViewTransformForKey(
    const ViewNode& root, std::string_view key) noexcept
{
    TransformMatch match;
    FindTransform(root, key, {}, std::nullopt, false, match);
    return match.found ? match.node : ViewResolvedTransform{};
}

std::optional<ViewRect> ResolveViewClipForKey(
    const ViewNode& root, std::string_view key,
    bool includeMatchedNode) noexcept
{
    TransformMatch match;
    FindTransform(root, key, {}, std::nullopt, false, match);
    if (!match.found) return std::nullopt;
    return includeMatchedNode ? match.nodeClip : match.parentClip;
}

ViewRect ApplyViewTransform(const ViewRect& rect,
    const ViewResolvedTransform& transform) noexcept
{
    return { rect.x * transform.scale + transform.translateX,
        rect.y * transform.scale + transform.translateY,
        rect.width * transform.scale,
        rect.height * transform.scale };
}

void ApplyViewTransform(const ViewNode& root,
    InteractionRegion& region) noexcept
{
    const auto transform = ResolveViewTransformForKey(root, region.key);
    const auto applyShape = [&transform](InteractionShape& shape) {
        shape.x = shape.x * transform.scale + transform.translateX;
        shape.y = shape.y * transform.scale + transform.translateY;
        shape.width *= transform.scale;
        shape.height *= transform.scale;
        shape.radius *= transform.scale;
    };
    applyShape(region.shape);
    for (auto& fragment : region.hitFragments)
        applyShape(fragment);
    if (const auto clip = ResolveViewClipForKey(root, region.key, false))
    {
        region.clip = InteractionClipRect{
            clip->x, clip->y, clip->width, clip->height };
    }
    else region.clip.reset();
    if (region.controlKind == InteractionControlKind::Slider)
    {
        region.controlStart = region.controlStart * transform.scale +
            (region.vertical ? transform.translateY : transform.translateX);
        region.controlLength *= transform.scale;
    }
}

void ViewTransitionRuntime::BeginFrame() noexcept
{
    if (++generation_ == 0)
    {
        generation_ = 1;
        for (auto& [key, entry] : entries_)
        {
            (void)key;
            entry.generation = 0;
        }
    }
}

ViewStyle ViewTransitionRuntime::Resolve(std::string_view key,
    const ViewStyle& target,
    const std::optional<ViewTransition>& transition,
    TimePoint now, bool reducedMotion)
{
    if (key.empty()) return target;
    const ViewTransition configured = transition.value_or(ViewTransition{});
    auto [position, inserted] = entries_.try_emplace(std::string(key));
    Entry& entry = position->second;
    entry.generation = generation_;
    if (inserted)
    {
        entry.start = target;
        entry.target = target;
        entry.transition = configured;
        return target;
    }

    const auto presentation = [&entry, now]() {
        if (!entry.active || entry.transition.durationMilliseconds == 0)
            return entry.target;
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::duration<float, std::milli>>(
                now - entry.started).count();
        const float progress = elapsed /
            static_cast<float>(entry.transition.durationMilliseconds);
        return InterpolateTransitionStyle(entry.start, entry.target,
            entry.transition, progress);
    };
    const bool targetChanged = entry.target != target;
    const bool configurationChanged = entry.transition != configured;
    if (targetChanged)
    {
        entry.start = presentation();
        entry.target = target;
        entry.transition = configured;
        entry.started = now;
        entry.active = transition.has_value() && !reducedMotion &&
            HasTransitionDifference(
                entry.start, entry.target, entry.transition);
    }
    else if (configurationChanged || reducedMotion)
    {
        entry.transition = configured;
        entry.active = false;
        entry.start = target;
    }
    if (!entry.active) return target;
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::duration<float, std::milli>>(
            now - entry.started).count();
    if (elapsed >= static_cast<float>(
            entry.transition.durationMilliseconds))
    {
        entry.active = false;
        entry.start = target;
        return target;
    }
    return InterpolateTransitionStyle(entry.start, entry.target,
        entry.transition, elapsed /
            static_cast<float>(entry.transition.durationMilliseconds));
}

void ViewTransitionRuntime::EndFrame()
{
    std::erase_if(entries_, [this](const auto& item) {
        return item.second.generation != generation_;
    });
}

bool ViewTransitionRuntime::Tick(TimePoint now) noexcept
{
    bool hadActive = false;
    for (auto& [key, entry] : entries_)
    {
        (void)key;
        if (!entry.active) continue;
        hadActive = true;
        if (now - entry.started >= std::chrono::milliseconds(
                entry.transition.durationMilliseconds))
        {
            entry.active = false;
            entry.start = entry.target;
        }
    }
    return hadActive;
}

bool ViewTransitionRuntime::HasActive() const noexcept
{
    return std::any_of(entries_.begin(), entries_.end(),
        [](const auto& item) { return item.second.active; });
}

void ViewTransitionRuntime::Clear() noexcept
{
    entries_.clear();
    generation_ = 0;
}

std::size_t ViewTransitionRuntime::Size() const noexcept
{
    return entries_.size();
}

static bool InteractionRegionOverlapsClip(
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

std::vector<const ViewNode*> ViewChildrenInPaintOrder(const ViewNode& node)
{
    std::vector<const ViewNode*> result;
    result.reserve(node.children.size());
    for (const auto& child : node.children) result.push_back(&child);
    std::stable_sort(result.begin(), result.end(),
        [](const ViewNode* left, const ViewNode* right) {
            return left->zIndex < right->zIndex;
        });
    return result;
}

ViewRect ViewRadioOptionFrame(
    const ViewNode& node, std::size_t optionIndex) noexcept
{
    if (node.options.empty() || optionIndex >= node.options.size())
        return {};
    const ViewRect content = ContentRect(node);
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

ViewRect ViewSelectOptionFrame(const ViewNode& node,
    std::size_t optionIndex, float viewportHeight) noexcept
{
    if (node.options.empty() || optionIndex >= node.options.size())
        return {};
    const float optionHeight = std::max(28.0f,
        std::min(48.0f, node.fontSize * 1.8f));
    const float popupHeight = optionHeight *
        static_cast<float>(node.options.size());
    const float below = viewportHeight -
        (node.frame.y + node.frame.height);
    const bool openAbove = below < popupHeight &&
        node.frame.y >= popupHeight;
    const float popupTop = openAbove
        ? node.frame.y - popupHeight
        : node.frame.y + node.frame.height;
    return { node.frame.x,
        popupTop + optionHeight * static_cast<float>(optionIndex),
        node.frame.width, optionHeight };
}

ViewRect ViewMonthCalendarWeekdayFrame(
    const ViewNode& node, std::size_t weekdayIndex) noexcept
{
    if (weekdayIndex >= 7) return {};
    const ViewRect content = ContentRect(node);
    const float width = content.width / 7.0f;
    const float height = content.height / 7.0f;
    return { content.x + width * static_cast<float>(weekdayIndex),
        content.y, width, height };
}

ViewRect ViewMonthCalendarCellFrame(
    const ViewNode& node, std::size_t cellIndex) noexcept
{
    if (cellIndex >= 42) return {};
    const ViewRect content = ContentRect(node);
    const float width = content.width / 7.0f;
    const float headerHeight = content.height / 7.0f;
    const float height = (content.height - headerHeight) / 6.0f;
    const std::size_t column = cellIndex % 7;
    const std::size_t row = cellIndex / 7;
    return { content.x + width * static_cast<float>(column),
        content.y + headerHeight + height * static_cast<float>(row),
        width, height };
}

bool BuildViewMonthCalendarCells(const ViewNode& node,
    std::array<ViewMonthCalendarCell, 42>& cells, std::string& error)
{
    error.clear();
    cells = {};
    if (node.calendarYear < 1 || node.calendarYear > 9999 ||
        node.calendarMonth < 1 || node.calendarMonth > 12 ||
        node.firstDayOfWeek < 1 || node.firstDayOfWeek > 7)
    {
        error = "monthCalendar date fields are out of range";
        return false;
    }
    CivilDate first{ node.calendarYear, node.calendarMonth, 1, 0 };
    const long long firstSerial = DaysFromCivil(first.year,
        static_cast<unsigned>(first.month), 1);
    int firstWeekday = static_cast<int>((firstSerial + 4) % 7);
    if (firstWeekday < 0) firstWeekday += 7;
    first.weekday = firstWeekday + 1;
    const int leading =
        (first.weekday - node.firstDayOfWeek + 7) % 7;

    CivilDate selected;
    if (!node.calendarSelectedDate.empty() &&
        !ParseCivilDate(node.calendarSelectedDate, selected))
    {
        error = "monthCalendar selectedDate must be empty or ISO YYYY-MM-DD";
        return false;
    }
    CivilDate today;
    if (!node.calendarTodayDate.empty() &&
        !ParseCivilDate(node.calendarTodayDate, today))
    {
        error = "monthCalendar todayDate must be empty or ISO YYYY-MM-DD";
        return false;
    }
    std::unordered_set<std::string> eventDates;
    if (node.calendarEventDates.size() >
        ViewTreeLimits::MaximumCalendarEventDates)
    {
        error = "monthCalendar eventDates exceed 366 entries";
        return false;
    }
    for (const auto& value : node.calendarEventDates)
    {
        CivilDate eventDate;
        if (!ParseCivilDate(value, eventDate))
        {
            error = "monthCalendar eventDates must use ISO YYYY-MM-DD";
            return false;
        }
        if (!eventDates.insert(value).second)
        {
            error = "monthCalendar eventDates must be unique";
            return false;
        }
    }

    const long long gridStart = firstSerial - leading;
    for (std::size_t index = 0; index < cells.size(); ++index)
    {
        const CivilDate date = CivilFromDays(
            gridStart + static_cast<long long>(index));
        if (date.year < 1 || date.year > 9999)
        {
            error = "monthCalendar grid falls outside supported dates";
            cells = {};
            return false;
        }
        ViewMonthCalendarCell cell;
        cell.date = FormatCivilDate(date);
        cell.day = date.day;
        cell.currentMonth = date.year == node.calendarYear &&
            date.month == node.calendarMonth;
        cell.selected = cell.date == node.calendarSelectedDate;
        cell.today = cell.date == node.calendarTodayDate;
        cell.hasEvent = eventDates.contains(cell.date);
        cells[index] = std::move(cell);
    }
    return true;
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
    std::size_t collectionItems = 0;
    std::unordered_set<std::string> keys;
    std::unordered_set<std::string> resources;
    if (!ValidateNode(root, 0, nodes, textBytes, seriesPoints,
            collectionItems, keys, resources, std::nullopt, error))
        return false;
    ClearCollectionSelectionState(root);
    ApplyCollectionSelectionState(root);
    if (!ResolveGridPlacements(root, error)) return false;
    LayoutNode(root, { 0.0f, 0.0f, width, height });
    return ValidateTransforms(root, {}, error);
}

bool ValidateViewLogicalSlots(const ViewNode& root,
    const LogicalSlotModel& model, std::string& error)
{
    error.clear();
    std::unordered_set<std::string> surfaces;
    const auto validate = [&](const auto& self,
        const ViewNode& node) -> bool {
        if (node.type == ViewNodeType::SlotSurface)
        {
            if (!surfaces.insert(node.logicalSlotId).second)
            {
                error = "logical slot has more than one slotSurface: " +
                    node.logicalSlotId;
                return false;
            }
            const LogicalSlotSnapshot* snapshot =
                model.Find(node.logicalSlotId);
            if (!snapshot)
            {
                error = "slotSurface references an undeclared logical slot: " +
                    node.logicalSlotId;
                return false;
            }
            if (snapshot->kind != node.logicalSlotKind)
            {
                error = "slotSurface kind does not match its manifest declaration: " +
                    node.logicalSlotId;
                return false;
            }
            if (node.logicalSlotRevision != 0 &&
                node.logicalSlotRevision != snapshot->revision)
            {
                error = "slotSurface revision is stale: " +
                    node.logicalSlotId;
                return false;
            }

            std::vector<const ViewNode*> items;
            for (const auto& child : node.children)
                if (child.type == ViewNodeType::SlotItem)
                    items.push_back(&child);
            if (snapshot->kind == LogicalSlotKind::Binding)
            {
                if (snapshot->items.empty())
                {
                    if (!items.empty())
                    {
                        error = "empty binding slotSurface cannot report a slotItem: " +
                            node.logicalSlotId;
                        return false;
                    }
                }
                else if (items.size() != 1 || node.children.size() != 1 ||
                    items.front()->key != snapshot->items.front().id ||
                    items.front()->logicalSlotReference !=
                        snapshot->items.front().reference)
                {
                    error = "binding slotSurface must report its exact host item: " +
                        node.logicalSlotId;
                    return false;
                }
            }
            else
            {
                if (items.size() != snapshot->items.size() ||
                    node.children.size() != snapshot->items.size())
                {
                    error = "collection slotSurface must report every host item: " +
                        node.logicalSlotId;
                    return false;
                }
                for (std::size_t index = 0; index < items.size(); ++index)
                {
                    if (items[index]->key != snapshot->items[index].id ||
                        items[index]->logicalSlotReference !=
                            snapshot->items[index].reference)
                    {
                        error = "collection slotSurface item order or reference is stale: " +
                            node.logicalSlotId;
                        return false;
                    }
                }
            }
        }
        for (const auto& child : node.children)
            if (!self(self, child)) return false;
        return true;
    };
    return validate(validate, root);
}

bool CollectViewInteractionRegions(const ViewNode& root,
    std::vector<InteractionRegion>& regions, std::string& error)
{
    error.clear();
    regions.clear();
    if (!CollectRegions(root, regions, std::nullopt,
            root.frame.height, error)) return false;
    if (!CollectSelectOptions(root, regions, std::nullopt,
            root.frame.height, error)) return false;
    if (regions.size() > WidgetInteractionRegions::kMaximumRegions)
    {
        error = "view interaction region limit exceeded (256)";
        regions.clear();
        return false;
    }
    for (auto& region : regions)
        ApplyViewTransform(root, region);
    std::erase_if(regions, [](const auto& region) {
        return !InteractionRegionOverlapsClip(region);
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
        control.fontSize *= transform.scale;
        control.padding.top *= transform.scale;
        control.padding.right *= transform.scale;
        control.padding.bottom *= transform.scale;
        control.padding.left *= transform.scale;
    }
    std::erase_if(controls, [](const auto& control) {
        return control.clip && !Overlaps(control.frame, *control.clip);
    });
    return true;
}

bool ApplyViewScrollOffsets(ViewNode& root,
    const ViewScrollOffsetResolver& resolver,
    std::vector<ViewScrollViewport>& viewports, std::string& error)
{
    error.clear();
    viewports.clear();
    std::size_t scrollContainers = 0;
    if (!ApplyScrollState(root, resolver, viewports, std::nullopt,
            scrollContainers, error) || !ValidateTransforms(root, {}, error))
    {
        viewports.clear();
        return false;
    }
    for (auto& viewport : viewports)
    {
        if (const auto clip = ResolveViewClipForKey(
                root, viewport.key, true))
            viewport.frame = *clip;
        else
            viewport.frame = ApplyViewTransform(viewport.frame,
                ResolveViewTransformForKey(root, viewport.key));
    }
    std::erase_if(viewports, [](const auto& viewport) {
        return viewport.frame.width <= 0.0f ||
            viewport.frame.height <= 0.0f;
    });
    return true;
}

bool ComputeViewVirtualRange(std::size_t itemCount, float itemExtent,
    std::size_t columns, float rowGap, float viewportExtent,
    float requestedOffset, std::size_t overscan,
    ViewVirtualRange& range, std::string& error)
{
    error.clear();
    range = {};
    if (itemCount > ViewTreeLimits::MaximumVirtualItemCount ||
        columns == 0 || columns > 64 ||
        overscan > ViewTreeLimits::MaximumVirtualOverscan ||
        !FiniteInRange(itemExtent, 0.000001f, MaximumScrollExtent) ||
        !FiniteInRange(rowGap, 0.0f, 4096.0f) ||
        !FiniteInRange(viewportExtent, 0.000001f,
            MaximumScrollExtent) ||
        !std::isfinite(requestedOffset))
    {
        error = "virtual range arguments must be finite and within their limits";
        return false;
    }

    const std::size_t rowCount = itemCount == 0 ? 0 :
        (itemCount + columns - 1) / columns;
    const double rawContentExtent =
        static_cast<double>(rowCount) * itemExtent +
        static_cast<double>(rowCount > 0 ? rowCount - 1 : 0) * rowGap;
    if (!std::isfinite(rawContentExtent) ||
        rawContentExtent > MaximumScrollExtent)
    {
        error = "virtual collection content extent exceeds 1000000";
        return false;
    }
    range.viewportExtent = viewportExtent;
    range.contentExtent = std::max(viewportExtent,
        static_cast<float>(rawContentExtent));
    range.maximum = std::max(0.0f,
        range.contentExtent - viewportExtent);
    range.offset = std::clamp(requestedOffset, 0.0f, range.maximum);
    if (itemCount == 0) return true;

    const double stride = static_cast<double>(itemExtent) + rowGap;
    std::size_t firstVisibleRow = std::min(rowCount - 1,
        static_cast<std::size_t>(std::floor(range.offset / stride)));
    const double firstRowEnd =
        static_cast<double>(firstVisibleRow) * stride + itemExtent;
    if (firstRowEnd <= range.offset && firstVisibleRow + 1 < rowCount)
        ++firstVisibleRow;
    const double visibleEnd = std::nextafter(
        static_cast<double>(range.offset) + viewportExtent,
        -std::numeric_limits<double>::infinity());
    std::size_t lastVisibleRow = std::min(rowCount - 1,
        static_cast<std::size_t>(std::floor(
            std::max(0.0, visibleEnd) / stride)));
    if (lastVisibleRow < firstVisibleRow)
        lastVisibleRow = firstVisibleRow;
    const std::size_t firstRow = firstVisibleRow > overscan
        ? firstVisibleRow - overscan : 0;
    const std::size_t lastRow = std::min(rowCount - 1,
        lastVisibleRow + std::min(overscan,
            rowCount - 1 - lastVisibleRow));
    range.firstIndex = firstRow * columns + 1;
    range.lastIndex = std::min(itemCount, (lastRow + 1) * columns);
    if (range.lastIndex - range.firstIndex + 1 >
        ViewTreeLimits::MaximumVirtualWindowItems)
    {
        error = "virtual materialization window exceeds 128 items";
        range = {};
        return false;
    }
    return true;
}

const char* ViewNodeTypeName(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract ? contract->name.data() : "unknown";
}
}
