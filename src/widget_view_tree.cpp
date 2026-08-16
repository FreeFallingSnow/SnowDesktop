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
    node.virtualMeasuredExtents.clear();
    node.virtualMeasuredIndices.clear();
    if (node.estimatedItemSize)
    {
        float cursor = 0.0f;
        std::size_t previousIndex = 0;
        std::string rangeError;
        const float rowGap = node.rowGap.value_or(node.gap);
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
                        *node.estimatedItemSize, rowGap, itemIndex,
                        node.virtualMeasurements, cursor, rangeError))
                    return;
            }
            const float intrinsic = child.height.kind ==
                ViewLengthKind::Fixed
                ? child.height.value + ViewVerticalMargin(child)
                : std::max(1.0f, OuterIntrinsicHeight(child));
            const ResolvedNodeSize resolved = ResolveOuterNodeSize(
                child, content.width, intrinsic);
            const float measuredExtent = std::max(1.0f, resolved.height);
            LayoutNode(child, { content.x, content.y + cursor,
                resolved.width, measuredExtent });
            node.virtualMeasuredExtents.push_back(measuredExtent);
            node.virtualMeasuredIndices.push_back(itemIndex);
            cursor += measuredExtent + rowGap;
            previousIndex = itemIndex;
        }
        return;
    }
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

bool ValidateNode(const ViewNode& node, std::size_t depth,
    std::size_t& nodes, std::size_t& textBytes,
    std::size_t& seriesPoints, std::size_t& collectionItems,
    std::unordered_set<std::string>& keys,
    std::unordered_set<char>& accessKeys,
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
        std::array<bool, 6> seen{};
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
    const auto validatePresenceTransition = [&error](
        const std::optional<ViewPresenceTransition>& presence,
        std::string_view name) {
        if (!presence) return true;
        if (presence->durationMilliseconds < 1 ||
            presence->durationMilliseconds > 2000 ||
            (!presence->opacity && !presence->transform) ||
            (presence->opacity &&
                !FiniteInRange(*presence->opacity, 0.0f, 1.0f)))
        {
            error = "view " + std::string(name) +
                " requires bounded opacity or transform and a duration from 1 to 2000ms";
            return false;
        }
        switch (presence->easing)
        {
        case ViewTransitionEasing::Linear:
        case ViewTransitionEasing::EaseIn:
        case ViewTransitionEasing::EaseOut:
        case ViewTransitionEasing::EaseInOut:
            return true;
        default:
            error = "view " + std::string(name) +
                " easing is unsupported";
            return false;
        }
    };
    if (!validatePresenceTransition(
            node.enterTransition, "enterTransition") ||
        !validatePresenceTransition(
            node.exitTransition, "exitTransition"))
        return false;
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
        node.collectionContent != ViewCollectionContent::Items)
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
                    node.rowGap.value_or(node.gap), 1.0f, 0.0f,
                    node.overscan, node.virtualMeasurements,
                    range, error))
                return false;
        }
        else if (!ComputeViewVirtualRange(node.itemCount, node.itemExtent,
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
            eventName != "pointerUp" && eventName != "pointerMove" &&
            eventName != "wheel" && eventName != "keyDown" &&
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
                seriesPoints, collectionItems, keys, accessKeys, resources,
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
            region.tooltip = node.tooltip;
            region.events = node.events;
            region.controlKind = InteractionControlKind::Radio;
            region.checked = option.value == node.selectedValue;
            region.currentSelection = node.selectedValue;
            region.proposedSelection = option.value;
            region.accessibilityRole = "radio";
            region.accessibilityLabel = option.label;
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

void ApplyStickyHeaders(ViewNode& node, const ViewRect& viewport) noexcept
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden ||
        IsScrollContainer(node.type))
        return;
    if (node.type == ViewNodeType::List &&
        node.orientation == ViewOrientation::Vertical)
    {
        std::vector<ViewNode*> headers;
        for (auto& child : node.children)
        {
            if (child.visible &&
                child.visibility != ViewVisibility::Hidden && child.sticky)
                headers.push_back(&child);
        }
        const ViewRect content = ContentRect(node);
        const float listBottom = content.y + content.height;
        for (std::size_t index = 0; index < headers.size(); ++index)
        {
            ViewNode& header = *headers[index];
            const float originalY = header.frame.y;
            float presentedY = std::max(originalY, viewport.y);
            presentedY = std::min(presentedY,
                listBottom - header.frame.height);
            if (index + 1 < headers.size())
                presentedY = std::min(presentedY,
                    headers[index + 1]->frame.y - header.frame.height);
            if (presentedY > originalY + 0.001f)
            {
                TranslateTree(header, 0.0f, presentedY - originalY);
                header.stickyPresented = true;
            }
        }
    }
    for (auto& child : node.children)
        ApplyStickyHeaders(child, viewport);
}

void ApplyVirtualStickyHeaders(ViewNode& node,
    const ViewRect& viewport) noexcept
{
    if (node.type != ViewNodeType::VirtualList ||
        node.sectionHeaderIndices.empty() ||
        node.virtualChildIndices.size() != node.children.size())
        return;
    std::vector<ViewNode*> headers;
    headers.reserve(node.sectionHeaderIndices.size());
    for (std::size_t childIndex = 0;
        childIndex < node.children.size(); ++childIndex)
    {
        ViewNode& child = node.children[childIndex];
        if (child.visible && child.visibility != ViewVisibility::Hidden &&
            std::binary_search(node.sectionHeaderIndices.begin(),
                node.sectionHeaderIndices.end(),
                node.virtualChildIndices[childIndex]))
            headers.push_back(&child);
    }
    const float listBottom = viewport.y +
        node.scrollContentExtent - node.scrollOffset;
    for (std::size_t index = 0; index < headers.size(); ++index)
    {
        ViewNode& header = *headers[index];
        const float originalY = header.frame.y;
        float presentedY = std::max(originalY, viewport.y);
        presentedY = std::min(presentedY,
            listBottom - header.frame.height);
        if (index + 1 < headers.size())
            presentedY = std::min(presentedY,
                headers[index + 1]->frame.y - header.frame.height);
        if (presentedY > originalY + 0.001f)
        {
            TranslateTree(header, 0.0f, presentedY - originalY);
            header.stickyPresented = true;
        }
    }
}

const ViewNode* FindVisibleDescendantByKey(const ViewNode& node,
    std::string_view key) noexcept
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return nullptr;
    if (node.key == key) return &node;
    for (const auto& child : node.children)
    {
        if (const auto* found = FindVisibleDescendantByKey(child, key))
            return found;
    }
    return nullptr;
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
            if (node.estimatedItemSize)
            {
                if (!ComputeViewVariableVirtualRange(node.itemCount,
                        *node.estimatedItemSize,
                        node.rowGap.value_or(node.gap), viewportExtent,
                        0.0f, node.overscan, node.virtualMeasurements,
                        virtualRange, error))
                    return false;
            }
            else if (!ComputeViewVirtualRange(node.itemCount,
                    node.itemExtent,
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
        const std::optional<float> resolved = resolver
            ? resolver(node.key, maximum) : std::optional<float>{};
        float requested = resolved.value_or(0.0f);
        bool initialized = !resolved.has_value();
        if (initialized && IsVirtualCollection(node.type) &&
            HasCollectionPlaceholder(node) && node.initialScrollIndex)
            initialized = false;
        if (initialized && node.type == ViewNodeType::Scroll &&
            node.initialScrollKey)
        {
            const ViewNode* target = FindVisibleDescendantByKey(
                node.children.front(), *node.initialScrollKey);
            if (!target)
            {
                error = "view initialScrollKey does not reference a visible descendant";
                return false;
            }
            const float targetStart = vertical
                ? target->frame.y - clip.y : target->frame.x - clip.x;
            const float targetEnd = targetStart +
                (vertical ? target->frame.height : target->frame.width);
            if (targetStart < 0.0f) requested = targetStart;
            else if (targetEnd > viewportExtent)
                requested = targetEnd - viewportExtent;
        }
        else if (initialized && IsVirtualCollection(node.type) &&
            !HasCollectionPlaceholder(node) && node.initialScrollIndex)
        {
            if (node.estimatedItemSize)
            {
                if (!ComputeViewVariableVirtualItemScrollOffset(
                        node.itemCount, *node.estimatedItemSize,
                        node.rowGap.value_or(node.gap), viewportExtent,
                        0.0f, *node.initialScrollIndex, "nearest",
                        node.virtualMeasurements, requested, error))
                    return false;
            }
            else if (!ComputeViewVirtualItemScrollOffset(node.itemCount,
                    node.itemExtent,
                    node.type == ViewNodeType::VirtualGrid
                        ? node.columns : 1,
                    node.rowGap.value_or(node.gap), viewportExtent,
                    0.0f, *node.initialScrollIndex, "nearest",
                    requested, error))
                return false;
        }
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
            if (node.estimatedItemSize)
            {
                if (!ComputeViewVariableVirtualRange(node.itemCount,
                        *node.estimatedItemSize,
                        node.rowGap.value_or(node.gap), viewportExtent,
                        offset, 0, node.virtualMeasurements,
                        visibleRange, error))
                    return false;
            }
            else if (!ComputeViewVirtualRange(node.itemCount, node.itemExtent,
                    node.type == ViewNodeType::VirtualGrid
                        ? node.columns : 1,
                    node.rowGap.value_or(node.gap), viewportExtent,
                    offset, 0, visibleRange, error))
                return false;
            if (node.itemCount > 0)
            {
                std::optional<std::size_t> expectedStickyHeader;
                const auto nextHeader = std::upper_bound(
                    node.sectionHeaderIndices.begin(),
                    node.sectionHeaderIndices.end(),
                    visibleRange.firstIndex);
                if (nextHeader != node.sectionHeaderIndices.begin())
                    expectedStickyHeader = *std::prev(nextHeader);
                if (node.stickyHeaderIndex != expectedStickyHeader)
                {
                    error = "virtualList stickyHeaderIndex does not match the current visible range";
                    return false;
                }
                const std::size_t windowChildOffset =
                    node.stickyHeaderIndex &&
                    *node.stickyHeaderIndex < node.firstIndex ? 1 : 0;
                const std::size_t windowChildCount =
                    node.children.size() - windowChildOffset;
                const std::size_t providedLast = node.firstIndex +
                    windowChildCount - 1;
                if (node.firstIndex > visibleRange.firstIndex ||
                    providedLast < visibleRange.lastIndex)
                {
                    error = "virtual collection window does not cover the visible range";
                    return false;
                }
            }
            for (auto& child : node.children)
                TranslateTree(child, 0.0f, -offset);
            ApplyVirtualStickyHeaders(node, clip);
        }
        else if (!IsVirtualCollection(node.type))
        {
            TranslateTree(node.children.front(),
                vertical ? 0.0f : -offset,
                vertical ? -offset : 0.0f);
            if (vertical)
                ApplyStickyHeaders(node.children.front(), clip);
        }

        childClip = IntersectRects(inheritedClip, clip);
        const ViewRect visibleFrame = childClip.value_or(clip);
        viewports.push_back({ node.key, visibleFrame, node.orientation,
            viewportExtent, contentExtent, offset, maximum, initialized });
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
        local.m11 * parent.m11 + local.m12 * parent.m21,
        local.m11 * parent.m12 + local.m12 * parent.m22,
        local.m21 * parent.m11 + local.m22 * parent.m21,
        local.m21 * parent.m12 + local.m22 * parent.m22,
        local.dx * parent.m11 + local.dy * parent.m21 + parent.dx,
        local.dx * parent.m12 + local.dy * parent.m22 + parent.dy,
    };
}

ViewResolvedTransform LocalTransform(const ViewRect& frame,
    const std::optional<ViewTransform>& transform) noexcept
{
    if (!transform) return {};
    const auto& value = *transform;
    const float originX = frame.x + frame.width * value.originX;
    const float originY = frame.y + frame.height * value.originY;
    const float radians = value.rotate *
        (3.14159265358979323846f / 180.0f);
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const float skewX = std::tan(value.skewX *
        (3.14159265358979323846f / 180.0f));
    const float skewY = std::tan(value.skewY *
        (3.14159265358979323846f / 180.0f));
    const float scaleX = value.scale * value.scaleX;
    const float scaleY = value.scale * value.scaleY;
    const ViewResolvedTransform result{
        scaleX * (cosine - skewY * sine),
        scaleX * (sine + skewY * cosine),
        scaleY * (skewX * cosine - sine),
        scaleY * (skewX * sine + cosine),
        0.0f,
        0.0f,
    };
    return { result.m11, result.m12, result.m21, result.m22,
        originX - originX * result.m11 - originY * result.m21 +
            value.translateX,
        originY - originX * result.m12 - originY * result.m22 +
            value.translateY };
}

ViewResolvedTransform LocalTransform(const ViewNode& node) noexcept
{
    return LocalTransform(node.frame, node.transform);
}

struct TransformedPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

TransformedPoint ApplyTransform(float x, float y,
    const ViewResolvedTransform& transform) noexcept
{
    return { x * transform.m11 + y * transform.m21 + transform.dx,
        x * transform.m12 + y * transform.m22 + transform.dy };
}

float TransformScaleX(const ViewResolvedTransform& transform) noexcept
{
    return std::hypot(transform.m11, transform.m12);
}

float TransformScaleY(const ViewResolvedTransform& transform) noexcept
{
    return std::hypot(transform.m21, transform.m22);
}

bool IsPositiveAxisAlignedTransform(
    const ViewResolvedTransform& transform) noexcept
{
    constexpr float epsilon = 0.00001f;
    return transform.m11 > epsilon && transform.m22 > epsilon &&
        std::abs(transform.m12) <= epsilon &&
        std::abs(transform.m21) <= epsilon;
}

bool IsAxisAlignedTransform(const ViewResolvedTransform& transform) noexcept
{
    constexpr float epsilon = 0.00001f;
    return (std::abs(transform.m12) <= epsilon &&
            std::abs(transform.m21) <= epsilon) ||
        (std::abs(transform.m11) <= epsilon &&
            std::abs(transform.m22) <= epsilon);
}

bool SingularValues(const ViewResolvedTransform& transform,
    float& minimum, float& maximum) noexcept
{
    const double squaredTrace =
        static_cast<double>(transform.m11) * transform.m11 +
        static_cast<double>(transform.m12) * transform.m12 +
        static_cast<double>(transform.m21) * transform.m21 +
        static_cast<double>(transform.m22) * transform.m22;
    const double determinant =
        static_cast<double>(transform.m11) * transform.m22 -
        static_cast<double>(transform.m12) * transform.m21;
    const double discriminant = std::sqrt(std::max(
        0.0, squaredTrace * squaredTrace -
            4.0 * determinant * determinant));
    const double maximumSquared = (squaredTrace + discriminant) * 0.5;
    if (!std::isfinite(maximumSquared) || maximumSquared <= 0.0)
        return false;
    maximum = static_cast<float>(std::sqrt(maximumSquared));
    minimum = static_cast<float>(std::abs(determinant) / maximum);
    return std::isfinite(minimum) && std::isfinite(maximum);
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

enum class ViewPresencePhase
{
    Target,
    Enter,
    Exit,
};

bool ValidateTransforms(const ViewNode& node,
    const ViewResolvedTransform& parent, std::string& error,
    ViewPresencePhase phase = ViewPresencePhase::Target)
{
    const ViewPresenceTransition* presence = nullptr;
    if (phase == ViewPresencePhase::Enter && node.enterTransition)
        presence = &*node.enterTransition;
    else if (phase == ViewPresencePhase::Exit && node.exitTransition)
        presence = &*node.exitTransition;
    const std::optional<ViewTransform>& transform =
        presence && presence->transform
        ? presence->transform : node.transform;
    if (transform)
    {
        const auto& value = *transform;
        if (!FiniteInRange(value.translateX,
                -MaximumDimension, MaximumDimension) ||
            !FiniteInRange(value.translateY,
                -MaximumDimension, MaximumDimension) ||
            !FiniteInRange(value.scale, 0.05f, 8.0f) ||
            !FiniteInRange(value.scaleX, 0.05f, 8.0f) ||
            !FiniteInRange(value.scaleY, 0.05f, 8.0f) ||
            !FiniteInRange(value.scale * value.scaleX, 0.05f, 8.0f) ||
            !FiniteInRange(value.scale * value.scaleY, 0.05f, 8.0f) ||
            !FiniteInRange(value.rotate, -360.0f, 360.0f) ||
            !FiniteInRange(value.skewX, -80.0f, 80.0f) ||
            !FiniteInRange(value.skewY, -80.0f, 80.0f) ||
            !FiniteInRange(value.originX, 0.0f, 1.0f) ||
            !FiniteInRange(value.originY, 0.0f, 1.0f))
        {
            error = phase == ViewPresencePhase::Enter
                ? "view enterTransition transform fields are outside their limits"
                : (phase == ViewPresencePhase::Exit
                    ? "view exitTransition transform fields are outside their limits"
                    : "view transform fields are outside their limits");
            return false;
        }
    }
    const ViewResolvedTransform current =
        ComposeTransform(LocalTransform(node.frame, transform), parent);
    float minimumScale = 0.0f;
    float maximumScale = 0.0f;
    if (!SingularValues(current, minimumScale, maximumScale) ||
        !FiniteInRange(minimumScale, 1.0f / 64.0f, 64.0f) ||
        !FiniteInRange(maximumScale, 1.0f / 64.0f, 64.0f))
    {
        error = "view cumulative transform axes must remain between 1/64 and 64";
        return false;
    }
    const ViewRect transformed = ApplyViewTransform(node.frame, current);
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
    if (node.clipFrame && !IsAxisAlignedTransform(current))
    {
        error = "view rotated clips are not supported";
        return false;
    }
    if ((IsInputNode(node.type) || IsScrollContainer(node.type) ||
            node.type == ViewNodeType::SlotSurface ||
            node.type == ViewNodeType::SlotItem) &&
        !IsPositiveAxisAlignedTransform(current))
    {
        error = "view host-managed controls and logical slots require an axis-aligned transform";
        return false;
    }
    for (const auto& child : node.children)
        if (!ValidateTransforms(child, current, error, phase))
            return false;
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

bool HasTransitionDifference(const ViewTransitionPresentation& start,
    const ViewTransitionPresentation& target,
    const ViewTransition& transition) noexcept
{
    const auto changedColor = [](const auto& first, const auto& second) {
        return first && second && first != second;
    };
    return (HasTransitionProperty(transition,
                ViewTransitionProperty::Background) &&
            changedColor(start.style.background, target.style.background)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Foreground) &&
            changedColor(start.style.foreground, target.style.foreground)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::BorderColor) &&
            changedColor(start.style.borderColor,
                target.style.borderColor)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Opacity) &&
            start.style.opacity.value_or(1.0f) !=
                target.style.opacity.value_or(1.0f)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Transform) &&
            start.transform.value_or(ViewTransform{}) !=
                target.transform.value_or(ViewTransform{})) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Layout) &&
            start.layoutFrame && target.layoutFrame &&
            start.layoutFrame != target.layoutFrame);
}

float InterpolateTransitionAngle(float start, float target,
    float progress) noexcept
{
    float delta = std::fmod(target - start, 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    else if (delta < -180.0f) delta += 360.0f;
    return start + delta * progress;
}

ViewTransform InterpolateTransitionTransform(const ViewTransform& start,
    const ViewTransform& target, float progress) noexcept
{
    const auto interpolate = [progress](float from, float to) {
        return from + (to - from) * progress;
    };
    return {
        interpolate(start.translateX, target.translateX),
        interpolate(start.translateY, target.translateY),
        interpolate(start.scale, target.scale),
        interpolate(start.originX, target.originX),
        interpolate(start.originY, target.originY),
        interpolate(start.scaleX, target.scaleX),
        interpolate(start.scaleY, target.scaleY),
        InterpolateTransitionAngle(start.rotate, target.rotate, progress),
        interpolate(start.skewX, target.skewX),
        interpolate(start.skewY, target.skewY),
    };
}

ViewTransitionPresentation InterpolateTransitionPresentation(
    const ViewTransitionPresentation& start,
    const ViewTransitionPresentation& target,
    const ViewTransition& transition,
    float progress) noexcept
{
    ViewTransitionPresentation result = target;
    progress = ResolveTransitionProgress(transition.easing, progress);
    const auto applyColor = [progress](const auto& from, const auto& to,
        auto& output) {
        if (from && to)
            output = InterpolateTransitionColor(*from, *to, progress);
    };
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Background))
        applyColor(start.style.background, target.style.background,
            result.style.background);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Foreground))
        applyColor(start.style.foreground, target.style.foreground,
            result.style.foreground);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::BorderColor))
        applyColor(start.style.borderColor, target.style.borderColor,
            result.style.borderColor);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Opacity))
    {
        const float from = start.style.opacity.value_or(1.0f);
        const float to = target.style.opacity.value_or(1.0f);
        result.style.opacity = from + (to - from) * progress;
    }
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Transform))
    {
        result.transform = InterpolateTransitionTransform(
            start.transform.value_or(ViewTransform{}),
            target.transform.value_or(ViewTransform{}), progress);
    }
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Layout) &&
        start.layoutFrame && target.layoutFrame)
    {
        const auto interpolate = [progress](float from, float to) {
            return from + (to - from) * progress;
        };
        result.layoutFrame = ViewRect{
            interpolate(start.layoutFrame->x, target.layoutFrame->x),
            interpolate(start.layoutFrame->y, target.layoutFrame->y),
            interpolate(start.layoutFrame->width,
                target.layoutFrame->width),
            interpolate(start.layoutFrame->height,
                target.layoutFrame->height),
        };
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

ViewResolvedTransform ResolveViewLocalTransform(
    const ViewNode& node) noexcept
{
    return LocalTransform(node);
}

ViewResolvedTransform ResolveViewLocalTransform(const ViewRect& frame,
    const std::optional<ViewTransform>& transform) noexcept
{
    return LocalTransform(frame, transform);
}

ViewResolvedTransform ResolveViewPresentationTransform(
    const ViewRect& renderedFrame,
    const ViewRect& targetLayoutFrame,
    const ViewRect& presentedLayoutFrame,
    const std::optional<ViewTransform>& transform) noexcept
{
    constexpr float epsilon = 0.000001f;
    if (renderedFrame.width <= epsilon || renderedFrame.height <= epsilon ||
        targetLayoutFrame.width <= epsilon ||
        targetLayoutFrame.height <= epsilon)
        return LocalTransform(renderedFrame, transform);

    const ViewRect presentedRenderedFrame{
        renderedFrame.x +
            (presentedLayoutFrame.x - targetLayoutFrame.x),
        renderedFrame.y +
            (presentedLayoutFrame.y - targetLayoutFrame.y),
        presentedLayoutFrame.width,
        presentedLayoutFrame.height,
    };
    const float scaleX = presentedRenderedFrame.width /
        renderedFrame.width;
    const float scaleY = presentedRenderedFrame.height /
        renderedFrame.height;
    const ViewResolvedTransform layout{
        scaleX, 0.0f, 0.0f, scaleY,
        presentedRenderedFrame.x - renderedFrame.x * scaleX,
        presentedRenderedFrame.y - renderedFrame.y * scaleY,
    };
    return ComposeTransform(layout,
        LocalTransform(presentedRenderedFrame, transform));
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
    const std::array points{
        ApplyTransform(rect.x, rect.y, transform),
        ApplyTransform(rect.x + rect.width, rect.y, transform),
        ApplyTransform(rect.x, rect.y + rect.height, transform),
        ApplyTransform(rect.x + rect.width,
            rect.y + rect.height, transform),
    };
    float left = points.front().x;
    float top = points.front().y;
    float right = points.front().x;
    float bottom = points.front().y;
    for (const auto& point : points)
    {
        left = std::min(left, point.x);
        top = std::min(top, point.y);
        right = std::max(right, point.x);
        bottom = std::max(bottom, point.y);
    }
    return { left, top, right - left, bottom - top };
}

void ApplyViewTransform(const ViewNode& root,
    InteractionRegion& region) noexcept
{
    const auto transform = ResolveViewTransformForKey(root, region.key);
    if (const auto clip = ResolveViewClipForKey(root, region.key, false))
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

std::uint32_t ResolveViewThemeColor(ViewThemeColorToken token,
    const ViewThemePalette& palette) noexcept
{
    switch (token)
    {
    case ViewThemeColorToken::WidgetBackground:
        return palette.widgetBackground;
    case ViewThemeColorToken::Surface:
        return palette.surface;
    case ViewThemeColorToken::SurfaceVariant:
        return palette.surfaceVariant;
    case ViewThemeColorToken::TextPrimary:
        return palette.textPrimary;
    case ViewThemeColorToken::TextSecondary:
        return palette.textSecondary;
    case ViewThemeColorToken::TextDisabled:
        return palette.textDisabled;
    case ViewThemeColorToken::Border:
        return palette.border;
    case ViewThemeColorToken::BorderStrong:
        return palette.borderStrong;
    case ViewThemeColorToken::SystemAccent:
        return palette.systemAccent;
    case ViewThemeColorToken::AccentText:
        return palette.accentText;
    case ViewThemeColorToken::Info:
        return palette.info;
    case ViewThemeColorToken::Success:
        return palette.success;
    case ViewThemeColorToken::Warning:
        return palette.warning;
    case ViewThemeColorToken::Error:
        return palette.error;
    }
    return palette.textPrimary;
}

std::optional<std::uint32_t> ResolveViewThemeColor(
    const std::optional<std::uint32_t>& literal,
    const std::optional<ViewThemeColorToken>& token,
    const ViewThemePalette& palette) noexcept
{
    if (token) return ResolveViewThemeColor(*token, palette);
    return literal;
}

ViewStyle ResolveViewThemeStyle(const ViewStyle& style,
    const ViewThemePalette& palette) noexcept
{
    ViewStyle result = style;
    result.background = ResolveViewThemeColor(
        style.background, style.backgroundToken, palette);
    result.foreground = ResolveViewThemeColor(
        style.foreground, style.foregroundToken, palette);
    result.borderColor = ResolveViewThemeColor(
        style.borderColor, style.borderColorToken, palette);
    result.backgroundToken.reset();
    result.foregroundToken.reset();
    result.borderColorToken.reset();
    return result;
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

ViewTransitionPresentation ViewTransitionRuntime::CurrentPresentation(
    const Entry& entry, TimePoint now) noexcept
{
    if (!entry.active ||
        entry.activeTransition.durationMilliseconds == 0)
        return entry.target;
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::duration<float, std::milli>>(
            now - entry.started).count();
    const float progress = elapsed /
        static_cast<float>(
            entry.activeTransition.durationMilliseconds);
    return InterpolateTransitionPresentation(entry.start, entry.target,
        entry.activeTransition, progress);
}

ViewTransitionPresentation ViewTransitionRuntime::ResolvePresentation(
    std::string_view key, const ViewStyle& targetStyle,
    const std::optional<ViewTransform>& targetTransform,
    const std::optional<ViewRect>& targetLayoutFrame,
    const std::optional<ViewTransition>& transition,
    const std::optional<ViewPresenceTransition>& enterTransition,
    TimePoint now, bool reducedMotion)
{
    const ViewTransitionPresentation target{
        targetStyle, targetTransform, targetLayoutFrame };
    if (key.empty()) return target;
    const ViewTransition configured = transition.value_or(ViewTransition{});
    auto [position, inserted] = entries_.try_emplace(std::string(key));
    Entry& entry = position->second;
    entry.generation = generation_;
    if (inserted)
    {
        entry.start = target;
        entry.target = target;
        entry.configuredTransition = configured;
        entry.activeTransition = configured;
        if (generation_ > 1 && enterTransition && !reducedMotion)
        {
            ViewTransition entering;
            entering.durationMilliseconds =
                enterTransition->durationMilliseconds;
            entering.easing = enterTransition->easing;
            if (enterTransition->opacity)
            {
                entry.start.style.opacity = enterTransition->opacity;
                entering.properties.push_back(
                    ViewTransitionProperty::Opacity);
            }
            if (enterTransition->transform)
            {
                entry.start.transform = enterTransition->transform;
                entering.properties.push_back(
                    ViewTransitionProperty::Transform);
            }
            entry.activeTransition = std::move(entering);
            entry.started = now;
            entry.active = HasTransitionDifference(entry.start,
                entry.target, entry.activeTransition);
            entry.entering = entry.active;
            if (entry.active) return entry.start;
        }
        return target;
    }

    const bool targetChanged = entry.target != target;
    const bool configurationChanged =
        entry.configuredTransition != configured;
    if (targetChanged)
    {
        entry.start = CurrentPresentation(entry, now);
        entry.target = target;
        entry.configuredTransition = configured;
        if (!entry.entering)
            entry.activeTransition = configured;
        entry.started = now;
        entry.active = !reducedMotion &&
            (entry.entering || transition.has_value()) &&
            HasTransitionDifference(
                entry.start, entry.target, entry.activeTransition);
        if (!entry.active) entry.entering = false;
    }
    else if (reducedMotion)
    {
        entry.configuredTransition = configured;
        entry.activeTransition = configured;
        entry.active = false;
        entry.entering = false;
        entry.start = target;
    }
    else if (configurationChanged)
    {
        entry.configuredTransition = configured;
        if (!entry.entering)
        {
            entry.activeTransition = configured;
            entry.active = false;
            entry.start = target;
        }
    }
    if (!entry.active) return target;
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::duration<float, std::milli>>(
            now - entry.started).count();
    if (elapsed >= static_cast<float>(
            entry.activeTransition.durationMilliseconds))
    {
        entry.active = false;
        entry.entering = false;
        entry.start = target;
        return target;
    }
    return InterpolateTransitionPresentation(entry.start, entry.target,
        entry.activeTransition, elapsed /
            static_cast<float>(
                entry.activeTransition.durationMilliseconds));
}

ViewStyle ViewTransitionRuntime::Resolve(std::string_view key,
    const ViewStyle& target,
    const std::optional<ViewTransition>& transition,
    TimePoint now, bool reducedMotion)
{
    return ResolvePresentation(key, target, std::nullopt,
        std::nullopt, transition, std::nullopt,
        now, reducedMotion).style;
}

void ViewTransitionRuntime::QueueExitTransitions(
    const ViewNode& previous, const ViewNode& current,
    TimePoint now, bool reducedMotion)
{
    std::unordered_set<std::string> currentKeys;
    const auto collectKeys = [&](const auto& self,
        const ViewNode& node) -> void {
        currentKeys.insert(node.key);
        for (const auto& child : node.children) self(self, child);
    };
    collectKeys(collectKeys, current);
    const auto pruneCurrent = [&currentKeys](const auto& self,
        ViewNode& node) -> void {
        std::erase_if(node.children,
            [&currentKeys](const ViewNode& child) {
                return currentKeys.contains(child.key);
            });
        for (auto& child : node.children) self(self, child);
    };
    std::erase_if(exits_, [&currentKeys](const ExitEntry& entry) {
        return currentKeys.contains(entry.node.key);
    });
    const auto countNodes = [](const auto& self,
        const ViewNode& node) -> std::size_t {
        std::size_t count = 1;
        for (const auto& child : node.children)
            count += self(self, child);
        return count;
    };
    std::size_t retainedNodes = 0;
    for (auto& exit : exits_)
    {
        pruneCurrent(pruneCurrent, exit.node);
        exit.nodeCount = countNodes(countNodes, exit.node);
        retainedNodes += exit.nodeCount;
    }
    if (reducedMotion)
    {
        exits_.clear();
        return;
    }

    std::unordered_set<std::string> exitingKeys;
    for (const auto& exit : exits_) exitingKeys.insert(exit.node.key);
    const auto collectExits = [&](const auto& self,
        const ViewNode& node,
        const ViewResolvedTransform& parentTransform,
        const std::optional<ViewRect>& inheritedClip,
        bool inheritedClipActive) -> void {
        if (!node.visible || node.visibility == ViewVisibility::Hidden ||
            node.frame.width <= 0.0f || node.frame.height <= 0.0f)
            return;
        const bool present = currentKeys.contains(node.key);
        if (!present && node.exitTransition &&
            !exitingKeys.contains(node.key))
        {
            ViewTransition descriptor;
            descriptor.durationMilliseconds =
                node.exitTransition->durationMilliseconds;
            descriptor.easing = node.exitTransition->easing;
            ViewTransitionPresentation start{
                node.style, node.transform, node.layoutTransitionFrame };
            if (const auto found = entries_.find(node.key);
                found != entries_.end())
                start = CurrentPresentation(found->second, now);
            ViewTransitionPresentation target = start;
            if (node.exitTransition->opacity)
            {
                target.style.opacity = node.exitTransition->opacity;
                descriptor.properties.push_back(
                    ViewTransitionProperty::Opacity);
            }
            if (node.exitTransition->transform)
            {
                target.transform = node.exitTransition->transform;
                descriptor.properties.push_back(
                    ViewTransitionProperty::Transform);
            }
            bool queued = false;
            if (HasTransitionDifference(start, target, descriptor))
            {
                ExitEntry exit;
                exit.node = node;
                pruneCurrent(pruneCurrent, exit.node);
                exit.nodeCount = countNodes(countNodes, exit.node);
                while (retainedNodes + exit.nodeCount >
                        ViewTreeLimits::MaximumNodes && !exits_.empty())
                {
                    retainedNodes -= exits_.front().nodeCount;
                    exitingKeys.erase(exits_.front().node.key);
                    exits_.erase(exits_.begin());
                }
                if (exit.nodeCount > ViewTreeLimits::MaximumNodes)
                    return;
                exit.parentTransform = parentTransform;
                exit.parentClip = inheritedClipActive
                    ? std::optional<ViewRect>(
                        inheritedClip.value_or(ViewRect{}))
                    : std::nullopt;
                exit.start = std::move(start);
                exit.target = std::move(target);
                exit.transition = std::move(descriptor);
                exit.started = now;
                retainedNodes += exit.nodeCount;
                exitingKeys.insert(exit.node.key);
                exits_.push_back(std::move(exit));
                queued = true;
            }
            if (queued) return;
        }

        const ViewResolvedTransform nodeTransform = ComposeTransform(
            LocalTransform(node), parentTransform);
        std::optional<ViewRect> childClip = inheritedClip;
        bool childClipActive = inheritedClipActive;
        if (node.clipFrame)
        {
            const ViewRect transformedClip =
                ApplyViewTransform(*node.clipFrame, nodeTransform);
            childClip = inheritedClipActive
                ? (inheritedClip
                    ? IntersectRects(inheritedClip, transformedClip)
                    : std::nullopt)
                : std::optional<ViewRect>(transformedClip);
            childClipActive = true;
        }
        for (const auto& child : node.children)
            self(self, child, nodeTransform,
                childClip, childClipActive);
    };
    collectExits(collectExits, previous, {}, std::nullopt, false);
}

std::vector<ViewExitTransitionFrame> ViewTransitionRuntime::ExitFrames(
    TimePoint now, bool reducedMotion)
{
    std::vector<ViewExitTransitionFrame> frames;
    if (reducedMotion)
    {
        exits_.clear();
        return frames;
    }
    frames.reserve(exits_.size());
    for (const auto& exit : exits_)
    {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::duration<float, std::milli>>(
                now - exit.started).count();
        const float progress = elapsed /
            static_cast<float>(exit.transition.durationMilliseconds);
        frames.push_back({ &exit.node, exit.parentTransform,
            exit.parentClip,
            InterpolateTransitionPresentation(exit.start, exit.target,
                exit.transition, progress) });
    }
    return frames;
}

void ViewTransitionRuntime::EndFrame()
{
    std::erase_if(entries_, [this](const auto& item) {
        return item.second.generation != generation_;
    });
}

bool ViewTransitionRuntime::Tick(TimePoint now) noexcept
{
    bool hadActive = !exits_.empty();
    for (auto& [key, entry] : entries_)
    {
        (void)key;
        if (!entry.active) continue;
        hadActive = true;
        if (now - entry.started >= std::chrono::milliseconds(
                entry.activeTransition.durationMilliseconds))
        {
            entry.active = false;
            entry.entering = false;
            entry.start = entry.target;
        }
    }
    std::erase_if(exits_, [now](const ExitEntry& exit) {
        return now - exit.started >= std::chrono::milliseconds(
            exit.transition.durationMilliseconds);
    });
    return hadActive;
}

bool ViewTransitionRuntime::HasActive() const noexcept
{
    return !exits_.empty() ||
        std::any_of(entries_.begin(), entries_.end(),
        [](const auto& item) { return item.second.active; });
}

void ViewTransitionRuntime::Clear() noexcept
{
    entries_.clear();
    exits_.clear();
    generation_ = 0;
}

std::size_t ViewTransitionRuntime::Size() const noexcept
{
    return entries_.size() + exits_.size();
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
            if (left->stickyPresented != right->stickyPresented)
                return !left->stickyPresented;
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
    const auto refreshVirtualChildIndices = [&](const auto& self,
        ViewNode& node) -> void {
        node.virtualChildIndices.clear();
        if (IsVirtualCollection(node.type) &&
            node.collectionContent == ViewCollectionContent::Items)
        {
            node.virtualChildIndices.reserve(node.children.size());
            const std::size_t windowChildOffset =
                node.stickyHeaderIndex &&
                *node.stickyHeaderIndex < node.firstIndex &&
                !node.children.empty() ? 1 : 0;
            if (windowChildOffset != 0)
                node.virtualChildIndices.push_back(
                    *node.stickyHeaderIndex);
            for (std::size_t child = windowChildOffset;
                child < node.children.size(); ++child)
                node.virtualChildIndices.push_back(
                    node.firstIndex + child - windowChildOffset);
        }
        for (auto& child : node.children) self(self, child);
    };
    refreshVirtualChildIndices(refreshVirtualChildIndices, root);

    std::size_t nodes = 0;
    std::size_t textBytes = 0;
    std::size_t seriesPoints = 0;
    std::size_t collectionItems = 0;
    std::unordered_set<std::string> keys;
    std::unordered_set<char> accessKeys;
    std::unordered_set<std::string> resources;
    if (!ValidateNode(root, 0, nodes, textBytes, seriesPoints,
            collectionItems, keys, accessKeys, resources,
            std::nullopt, error))
        return false;
    ClearCollectionSelectionState(root);
    ApplyCollectionSelectionState(root);
    if (!ResolveGridPlacements(root, error)) return false;
    LayoutNode(root, { 0.0f, 0.0f, width, height });
    CaptureLayoutTransitionFrames(root, std::nullopt);
    return ValidateTransforms(root, {}, error) &&
        ValidateTransforms(root, {}, error, ViewPresencePhase::Enter) &&
        ValidateTransforms(root, {}, error, ViewPresencePhase::Exit);
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
        const float scaleX = TransformScaleX(transform);
        const float scaleY = TransformScaleY(transform);
        control.fontSize *= scaleY;
        control.padding.top *= scaleY;
        control.padding.right *= scaleX;
        control.padding.bottom *= scaleY;
        control.padding.left *= scaleX;
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
            scrollContainers, error) ||
        !ValidateTransforms(root, {}, error) ||
        !ValidateTransforms(root, {}, error,
            ViewPresencePhase::Enter) ||
        !ValidateTransforms(root, {}, error,
            ViewPresencePhase::Exit))
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

bool ComputeViewVirtualItemScrollOffset(std::size_t itemCount,
    float itemExtent, std::size_t columns, float rowGap,
    float viewportExtent, float currentOffset, std::size_t index,
    std::string_view alignment, float& offset, std::string& error)
{
    offset = 0.0f;
    ViewVirtualRange range;
    if (!ComputeViewVirtualRange(itemCount, itemExtent, columns, rowGap,
            viewportExtent, currentOffset, 0, range, error))
        return false;
    if (index == 0 || index > itemCount)
    {
        error = "virtual item index is outside the collection";
        return false;
    }
    if (alignment != "nearest" && alignment != "start" &&
        alignment != "center" && alignment != "end")
    {
        error = "virtual item alignment must be nearest, start, center, or end";
        return false;
    }
    const std::size_t row = (index - 1) / columns;
    const float itemStart = static_cast<float>(row) *
        (itemExtent + rowGap);
    const float itemEnd = itemStart + itemExtent;
    float requested = range.offset;
    if (alignment == "start") requested = itemStart;
    else if (alignment == "center")
        requested = itemStart - (viewportExtent - itemExtent) * 0.5f;
    else if (alignment == "end")
        requested = itemEnd - viewportExtent;
    else if (itemStart < range.offset) requested = itemStart;
    else if (itemEnd > range.offset + viewportExtent)
        requested = itemEnd - viewportExtent;
    offset = std::clamp(requested, 0.0f, range.maximum);
    return true;
}

namespace
{
bool ValidateVariableVirtualMeasurements(std::size_t itemCount,
    float estimatedItemSize, float rowGap,
    std::span<const ViewVirtualItemMeasurement> measurements,
    float& contentExtent, std::string& error)
{
    if (itemCount > ViewTreeLimits::MaximumVirtualItemCount ||
        measurements.size() > 4096 ||
        !FiniteInRange(estimatedItemSize, 0.000001f,
            MaximumScrollExtent) ||
        !FiniteInRange(rowGap, 0.0f, 4096.0f))
    {
        error = "variable virtual collection arguments exceed their limits";
        return false;
    }
    std::unordered_set<std::size_t> indices;
    double total = static_cast<double>(itemCount) * estimatedItemSize +
        static_cast<double>(itemCount > 0 ? itemCount - 1 : 0) * rowGap;
    for (const auto& measurement : measurements)
    {
        if (measurement.index == 0 || measurement.index > itemCount ||
            !FiniteInRange(measurement.extent, 0.000001f,
                MaximumScrollExtent) ||
            !indices.insert(measurement.index).second)
        {
            error = "variable virtual measurements must use unique bounded 1-based indices";
            return false;
        }
        total += static_cast<double>(measurement.extent) -
            estimatedItemSize;
    }
    if (!std::isfinite(total) || total < 0.0 ||
        total > MaximumScrollExtent)
    {
        error = "variable virtual collection content extent exceeds 1000000";
        return false;
    }
    contentExtent = static_cast<float>(total);
    return true;
}

float VariableVirtualExtent(std::size_t index, float estimatedItemSize,
    std::span<const ViewVirtualItemMeasurement> measurements) noexcept
{
    const auto found = std::find_if(measurements.begin(), measurements.end(),
        [index](const auto& measurement) {
            return measurement.index == index;
        });
    return found == measurements.end()
        ? estimatedItemSize : found->extent;
}

float VariableVirtualStart(std::size_t index, float estimatedItemSize,
    float rowGap,
    std::span<const ViewVirtualItemMeasurement> measurements) noexcept
{
    double start = static_cast<double>(index - 1) *
        (estimatedItemSize + rowGap);
    for (const auto& measurement : measurements)
    {
        if (measurement.index < index)
            start += static_cast<double>(measurement.extent) -
                estimatedItemSize;
    }
    return static_cast<float>(start);
}
}

bool ComputeViewVariableVirtualRange(std::size_t itemCount,
    float estimatedItemSize, float rowGap, float viewportExtent,
    float requestedOffset, std::size_t overscan,
    std::span<const ViewVirtualItemMeasurement> measurements,
    ViewVirtualRange& range, std::string& error)
{
    error.clear();
    range = {};
    float logicalContentExtent = 0.0f;
    if (overscan > ViewTreeLimits::MaximumVirtualOverscan ||
        !FiniteInRange(viewportExtent, 0.000001f,
            MaximumScrollExtent) ||
        !std::isfinite(requestedOffset) ||
        !ValidateVariableVirtualMeasurements(itemCount,
            estimatedItemSize, rowGap, measurements,
            logicalContentExtent, error))
    {
        if (error.empty())
            error = "variable virtual range arguments must be finite and within their limits";
        return false;
    }
    range.viewportExtent = viewportExtent;
    range.contentExtent = std::max(viewportExtent, logicalContentExtent);
    range.maximum = std::max(0.0f,
        range.contentExtent - viewportExtent);
    range.offset = std::clamp(requestedOffset, 0.0f, range.maximum);
    if (itemCount == 0) return true;

    std::size_t low = 1;
    std::size_t high = itemCount;
    while (low < high)
    {
        const std::size_t middle = low + (high - low) / 2;
        const float end = VariableVirtualStart(middle,
            estimatedItemSize, rowGap, measurements) +
            VariableVirtualExtent(middle, estimatedItemSize, measurements);
        if (end <= range.offset) low = middle + 1;
        else high = middle;
    }
    const std::size_t firstVisible = low;
    const double visibleEnd = std::nextafter(
        static_cast<double>(range.offset) + viewportExtent,
        -std::numeric_limits<double>::infinity());
    low = firstVisible;
    high = itemCount + 1;
    while (low < high)
    {
        const std::size_t middle = low + (high - low) / 2;
        if (middle > itemCount ||
            VariableVirtualStart(middle, estimatedItemSize,
                rowGap, measurements) > visibleEnd)
            high = middle;
        else
            low = middle + 1;
    }
    const std::size_t lastVisible = std::max(firstVisible, low - 1);
    range.firstIndex = firstVisible > overscan
        ? firstVisible - overscan : 1;
    range.lastIndex = std::min(itemCount,
        lastVisible + std::min(overscan, itemCount - lastVisible));
    if (range.lastIndex - range.firstIndex + 1 >
        ViewTreeLimits::MaximumVirtualWindowItems)
    {
        error = "variable virtual materialization window exceeds 128 items";
        range = {};
        return false;
    }
    return true;
}

bool ComputeViewVariableVirtualItemStart(std::size_t itemCount,
    float estimatedItemSize, float rowGap, std::size_t index,
    std::span<const ViewVirtualItemMeasurement> measurements,
    float& start, std::string& error)
{
    start = 0.0f;
    float contentExtent = 0.0f;
    if (!ValidateVariableVirtualMeasurements(itemCount,
            estimatedItemSize, rowGap, measurements,
            contentExtent, error))
        return false;
    if (index == 0 || index > itemCount)
    {
        error = "variable virtual item index is outside the collection";
        return false;
    }
    start = VariableVirtualStart(index, estimatedItemSize,
        rowGap, measurements);
    return true;
}

bool ComputeViewVariableVirtualItemScrollOffset(std::size_t itemCount,
    float estimatedItemSize, float rowGap, float viewportExtent,
    float currentOffset, std::size_t index, std::string_view alignment,
    std::span<const ViewVirtualItemMeasurement> measurements,
    float& offset, std::string& error)
{
    offset = 0.0f;
    ViewVirtualRange range;
    if (!ComputeViewVariableVirtualRange(itemCount, estimatedItemSize,
            rowGap, viewportExtent, currentOffset, 0,
            measurements, range, error))
        return false;
    if (index == 0 || index > itemCount)
    {
        error = "variable virtual item index is outside the collection";
        return false;
    }
    if (alignment != "nearest" && alignment != "start" &&
        alignment != "center" && alignment != "end")
    {
        error = "variable virtual item alignment must be nearest, start, center, or end";
        return false;
    }
    const float itemStart = VariableVirtualStart(index,
        estimatedItemSize, rowGap, measurements);
    const float itemExtent = VariableVirtualExtent(index,
        estimatedItemSize, measurements);
    const float itemEnd = itemStart + itemExtent;
    float requested = range.offset;
    if (alignment == "start") requested = itemStart;
    else if (alignment == "center")
        requested = itemStart - (viewportExtent - itemExtent) * 0.5f;
    else if (alignment == "end") requested = itemEnd - viewportExtent;
    else if (itemStart < range.offset) requested = itemStart;
    else if (itemEnd > range.offset + viewportExtent)
        requested = itemEnd - viewportExtent;
    offset = std::clamp(requested, 0.0f, range.maximum);
    return true;
}

const char* ViewNodeTypeName(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract ? contract->name.data() : "unknown";
}
}
