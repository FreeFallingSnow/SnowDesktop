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

float ViewMarginExtent(const ViewNode& node) noexcept
{
    return node.margin * 2.0f;
}

ResolvedNodeSize ResolveOuterNodeSize(
    const ViewNode& node, float proposedWidth, float proposedHeight) noexcept
{
    const float margin = ViewMarginExtent(node);
    const ResolvedNodeSize inner = ResolveNodeSize(node,
        std::max(0.0f, proposedWidth - margin),
        std::max(0.0f, proposedHeight - margin));
    return { inner.width + margin, inner.height + margin };
}

float TextIntrinsicWidth(const ViewNode& node) noexcept
{
    const float approximateGlyphs = static_cast<float>(
        std::min<std::size_t>(node.text.size(), 256));
    const float spacing = std::max(0.0f, approximateGlyphs - 1.0f) *
        node.letterSpacing;
    return std::max(node.fontSize,
        approximateGlyphs * node.fontSize * 0.55f + spacing) +
        node.padding * 2.0f;
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

bool IsScrollContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Scroll || IsVirtualCollection(type);
}

const char* DefaultAccessibilityRole(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract ? contract->defaultAccessibilityRole.data() : "";
}

ViewRect ContentRect(const ViewNode& node) noexcept
{
    const float inset = std::min(node.padding,
        std::max(0.0f,
            std::min(node.frame.width, node.frame.height) * 0.5f));
    return { node.frame.x + inset, node.frame.y + inset,
        std::max(0.0f, node.frame.width - inset * 2.0f),
        std::max(0.0f, node.frame.height - inset * 2.0f) };
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
    return IntrinsicWidth(node) + ViewMarginExtent(node);
}

float OuterIntrinsicHeight(const ViewNode& node)
{
    return IntrinsicHeight(node) + ViewMarginExtent(node);
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
        return result + node.padding * 2.0f;
    }
    if (IsInputNode(node.type) || node.type == ViewNodeType::Select)
        return 144.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Slider)
        return (node.orientation == ViewOrientation::Horizontal
            ? 96.0f : 24.0f) + node.padding * 2.0f;
    if (node.type == ViewNodeType::Image ||
        node.type == ViewNodeType::ReferenceIcon)
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
    if (node.type == ViewNodeType::MonthCalendar)
        return 224.0f + node.padding * 2.0f;
    if (IsDataSeriesNode(node.type))
        return 64.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Spacer)
        return 0.0f;
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
            node.padding * 2.0f;
    }
    if (IsGridContainer(node.type))
    {
        std::vector<float> widths(node.columns, 0.0f);
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            widths[visible % node.columns] = std::max(
                widths[visible % node.columns], OuterIntrinsicWidth(child));
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
            result += OuterIntrinsicWidth(child);
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
    return result + node.padding * 2.0f;
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
        return TextIntrinsicLineHeight(node) + node.padding * 2.0f;
    if (node.type == ViewNodeType::Badge)
        return std::max(20.0f, TextIntrinsicLineHeight(node)) +
            node.padding * 2.0f;
    if (node.type == ViewNodeType::Image ||
        node.type == ViewNodeType::ReferenceIcon)
        return 48.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Button)
        return std::max(32.0f, TextIntrinsicLineHeight(node)) +
            node.padding * 2.0f;
    if (node.type == ViewNodeType::Toggle ||
        node.type == ViewNodeType::Checkbox)
        return std::max(32.0f, TextIntrinsicLineHeight(node)) +
            node.padding * 2.0f;
    if (node.type == ViewNodeType::RadioGroup)
    {
        const float optionHeight = std::max(
            32.0f, TextIntrinsicLineHeight(node));
        const float content = node.orientation == ViewOrientation::Vertical
            ? optionHeight * static_cast<float>(node.options.size()) +
                node.gap * static_cast<float>(
                    node.options.empty() ? 0 : node.options.size() - 1)
            : optionHeight;
        return content + node.padding * 2.0f;
    }
    if (node.type == ViewNodeType::TextArea)
        return 96.0f + node.padding * 2.0f;
    if (IsInputNode(node.type) || node.type == ViewNodeType::Select)
        return std::max(36.0f, node.fontSize * 1.8f) +
            node.padding * 2.0f;
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
    if (node.type == ViewNodeType::MonthCalendar)
        return 224.0f + node.padding * 2.0f;
    if (node.type == ViewNodeType::Spacer)
        return 0.0f;
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
            MaximumScrollExtent, extent)) + node.padding * 2.0f;
    }
    if (IsGridContainer(node.type))
    {
        std::vector<float> heights;
        std::size_t visible = 0;
        for (const auto& child : node.children)
        {
            if (!child.visible) continue;
            const std::size_t row = visible / node.columns;
            if (row >= heights.size()) heights.push_back(0.0f);
            heights[row] = std::max(
                heights[row], OuterIntrinsicHeight(child));
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
            result += OuterIntrinsicHeight(child);
            ++visible;
        }
        if (visible > 1)
            result += node.rowGap.value_or(node.gap) *
                static_cast<float>(visible - 1);
        return result + node.padding * 2.0f;
    }
    float result = 0.0f;
    if (node.type == ViewNodeType::Column ||
        node.type == ViewNodeType::List)
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

float ResolveOuterCrossSize(const ViewNode& node,
    const ViewLength& length, float intrinsic, float available,
    ViewAlignment alignment) noexcept
{
    const float margin = ViewMarginExtent(node);
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
        mainSizes[index] = (horizontal
            ? ConstrainWidth(child, innerSize)
            : ConstrainHeight(child, innerSize)) + ViewMarginExtent(child);
        baseTotal += mainSizes[index];
        growWeights[index] = child.flexGrow > 0.0f
            ? child.flexGrow :
            (mainLength.kind == ViewLengthKind::Fill ? 1.0f : 0.0f);
        growTotal += growWeights[index];
        shrinkWeights[index] = child.flexShrink *
            std::max(1.0f,
                mainSizes[index] - ViewMarginExtent(child));
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
                    ViewMarginExtent(child);
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
            crossLength, intrinsicCross, availableCross, alignment);
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
            horizontal ? mainSizes[index] : proposedCross,
            horizontal ? proposedCross : mainSizes[index]);
        mainSizes[index] = horizontal ? resolved.width : resolved.height;
        crossSizes[index] = horizontal ? resolved.height : resolved.width;
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
        const ViewAlignment alignment = child.alignSelf == ViewAlignment::Auto
            ? node.alignItems : child.alignSelf;
        const float crossOffset = AlignOffset(
            alignment, availableCross, crossSizes[index]);
        ViewRect childFrame;
        if (horizontal)
            childFrame = { cursor, content.y + crossOffset,
                mainSizes[index], crossSizes[index] };
        else
            childFrame = { content.x + crossOffset, cursor,
                crossSizes[index], mainSizes[index] };
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
            rowHeights[index / columns],
            OuterIntrinsicHeight(*visible[index]));

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
        const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
            ResolveOuterCrossSize(child, child.width, IntrinsicWidth(child),
                cellWidth, alignment),
            ResolveOuterCrossSize(child, child.height, IntrinsicHeight(child),
                rowHeights[row], alignment));
        const float x = content.x +
            static_cast<float>(column) * (cellWidth + columnGap) +
            AlignOffset(alignment, cellWidth, resolved.width);
        LayoutNode(child, { x,
            y + AlignOffset(alignment, rowHeights[row], resolved.height),
            resolved.width, resolved.height });
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
        float width = OuterIntrinsicWidth(child);
        if (child.width.kind == ViewLengthKind::Fixed)
            width = child.width.value + ViewMarginExtent(child);
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
            const ResolvedNodeSize resolved = ResolveOuterNodeSize(child,
                item.width, ResolveOuterCrossSize(child, child.height,
                    std::max(0.0f, item.intrinsicHeight -
                        ViewMarginExtent(child)),
                    line.height, alignment));
            LayoutNode(child, { x,
                y + AlignOffset(alignment, line.height, resolved.height),
                resolved.width, resolved.height });
            x += resolved.width + dynamicGap;
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
            IntrinsicWidth(child), content.width, alignment);
        float height = child.height.kind == ViewLengthKind::Fixed
            ? child.height.value + ViewMarginExtent(child) :
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
            IntrinsicHeight(child), content.height, alignment);
        float width = child.width.kind == ViewLengthKind::Fixed
            ? child.width.value + ViewMarginExtent(child) :
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
    const float margin = std::min(node.margin,
        std::max(0.0f,
            std::min(frame.width, frame.height) * 0.5f));
    const ResolvedNodeSize resolved = ResolveNodeSize(
        node, std::max(0.0f, frame.width - margin * 2.0f),
        std::max(0.0f, frame.height - margin * 2.0f));
    node.frame = { frame.x + margin, frame.y + margin,
        resolved.width, resolved.height };
    node.clipFrame.reset();
    node.scrollOffset = 0.0f;
    node.scrollViewportExtent = 0.0f;
    node.scrollContentExtent = 0.0f;
    const float inset = std::min(node.padding,
        std::max(0.0f,
            std::min(node.frame.width, node.frame.height) * 0.5f));
    const ViewRect content{
        node.frame.x + inset,
        node.frame.y + inset,
        std::max(0.0f, node.frame.width - inset * 2.0f),
        std::max(0.0f, node.frame.height - inset * 2.0f),
    };
    if (node.type == ViewNodeType::Row)
        LayoutLinear(node, content, true);
    else if (node.type == ViewNodeType::Column ||
        node.type == ViewNodeType::List)
        LayoutLinear(node, content, false);
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
        for (auto& child : node.children)
        {
            if (!child.visible) continue;
            const ViewAlignment alignment =
                child.alignSelf == ViewAlignment::Auto
                ? ViewAlignment::Stretch : child.alignSelf;
            const ResolvedNodeSize childSize = ResolveOuterNodeSize(child,
                ResolveOuterCrossSize(child, child.width,
                    IntrinsicWidth(child),
                    content.width, alignment),
                ResolveOuterCrossSize(child, child.height,
                    IntrinsicHeight(child),
                    content.height, alignment));
            LayoutNode(child, {
                content.x + AlignOffset(alignment,
                    content.width, childSize.width),
                content.y + AlignOffset(alignment,
                    content.height, childSize.height),
                childSize.width, childSize.height });
        }
    }
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
        !FiniteInRange(node.margin, 0.0f, 4096.0f) ||
        !FiniteInRange(node.padding, 0.0f, 4096.0f) ||
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
        if (!std::all_of(node.children.begin(), node.children.end(),
                [](const ViewNode& child) {
                    return child.type == ViewNodeType::ListItem;
                }))
        {
            error = std::string(ViewNodeTypeName(node.type)) +
                " children must all be listItem nodes";
            return false;
        }
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
        if (node.itemCount == 0)
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
            node.validationMessage.size() >
            ViewTreeLimits::MaximumTotalTextBytes)
    {
        error = "view tree text limit exceeded";
        return false;
    }
    textBytes += node.text.size() + node.inputValue.size() +
        node.placeholder.size() + node.alt.size() + node.tooltip.size() +
        node.validationMessage.size();
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
        for (const auto& span : node.spans)
        {
            if (span.text.empty() ||
                (span.fontSize &&
                    !FiniteInRange(*span.fontSize, 1.0f, 512.0f)))
            {
                error = "styledText spans require non-empty bounded text and font sizes";
                return false;
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
            eventName != "pointerUp" && eventName != "change" &&
            eventName != "focus" && eventName != "blur" &&
            eventName != "submit")
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
    if (node.type == ViewNodeType::Select)
    {
        if (!node.events.contains("change") ||
            !node.events.contains("click"))
        {
            error = "select nodes require click and change";
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

bool CollectRegions(const ViewNode& node,
    std::vector<InteractionRegion>& regions,
    const std::optional<ViewRect>& inheritedClip, float viewportHeight,
    std::string& error)
{
    if (!node.visible) return true;
    if (node.type == ViewNodeType::MonthCalendar)
    {
        std::array<ViewMonthCalendarCell, 42> cells;
        if (!BuildViewMonthCalendarCells(node, cells, error)) return false;
        std::map<std::string, InteractionAction, std::less<>> surfaceEvents;
        for (const auto& [name, action] : node.events)
            if (name != "change") surfaceEvents.emplace(name, action);
        if ((!surfaceEvents.empty() || !node.tooltip.empty()) &&
            (!inheritedClip || Overlaps(node.frame, *inheritedClip)))
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
            regions.push_back(std::move(surface));
        }
        for (std::size_t index = 0; index < cells.size(); ++index)
        {
            const auto& cell = cells[index];
            if (!cell.currentMonth && !node.showAdjacentDates) continue;
            const ViewRect frame = ViewMonthCalendarCellFrame(node, index);
            if (inheritedClip && !Overlaps(frame, *inheritedClip)) continue;
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
            if (inheritedClip && !Overlaps(frame, *inheritedClip))
                continue;
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
            regions.push_back(std::move(region));
        }
        return true;
    }
    if (IsInputNode(node.type))
    {
        if (!inheritedClip || Overlaps(node.frame, *inheritedClip))
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
                    name != "blur" && name != "submit")
                    region.events.emplace(name, action);
            region.accessibilityRole = node.accessibilityRole.empty()
                ? DefaultAccessibilityRole(node.type)
                : node.accessibilityRole;
            region.accessibilityLabel = node.accessibilityLabel;
            region.enabled = node.enabled;
            regions.push_back(std::move(region));
        }
        return true;
    }
    if (node.type == ViewNodeType::Select)
    {
        if (regions.size() >= WidgetInteractionRegions::kMaximumRegions)
        {
            error = "view interaction region limit exceeded (256)";
            return false;
        }
        if (!inheritedClip || Overlaps(node.frame, *inheritedClip))
        {
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
            regions.push_back(std::move(trigger));
        }
        return true;
    }
    if ((!node.events.empty() || !node.tooltip.empty() ||
            IsButtonNode(node.type) ||
            node.type == ViewNodeType::ListItem ||
            node.type == ViewNodeType::SlotItem) &&
        (!inheritedClip || Overlaps(node.frame, *inheritedClip)))
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
                node.events.contains("click"))
            ? "hand" : node.cursor;
        region.tooltip = node.tooltip;
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
            ? DefaultAccessibilityRole(node.type)
            : node.accessibilityRole;
        region.accessibilityLabel = node.accessibilityLabel.empty()
            ? node.text : node.accessibilityLabel;
        region.enabled = node.enabled;
        regions.push_back(std::move(region));
    }
    std::optional<ViewRect> childClip = inheritedClip;
    if (IsScrollContainer(node.type) && node.clipFrame)
    {
        if (inheritedClip && !Overlaps(*node.clipFrame, *inheritedClip))
            return true;
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    }
    for (const auto& child : node.children)
        if (!CollectRegions(child, regions, childClip,
                viewportHeight, error)) return false;
    return true;
}

bool CollectSelectOptions(const ViewNode& node,
    std::vector<InteractionRegion>& regions,
    const std::optional<ViewRect>& inheritedClip, float viewportHeight,
    std::string& error)
{
    if (!node.visible) return true;
    if (node.type == ViewNodeType::Select && node.expanded)
    {
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            const auto& option = node.options[index];
            const ViewRect frame = ViewSelectOptionFrame(
                node, index, viewportHeight);
            if (inheritedClip && !Overlaps(frame, *inheritedClip))
                continue;
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
            regions.push_back(std::move(region));
        }
        return true;
    }
    std::optional<ViewRect> childClip = inheritedClip;
    if (IsScrollContainer(node.type) && node.clipFrame)
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    for (const auto& child : node.children)
        if (!CollectSelectOptions(child, regions, childClip,
                viewportHeight, error)) return false;
    return true;
}

bool CollectInputs(const ViewNode& node,
    std::vector<ViewInputControl>& controls,
    const std::optional<ViewRect>& inheritedClip, std::string& error)
{
    if (!node.visible) return true;
    if (IsInputNode(node.type))
    {
        if (inheritedClip && !Overlaps(node.frame, *inheritedClip))
            return true;
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
        control.readOnly = node.readOnly;
        control.selectAll = node.selectAll;
        control.liveUpdate = node.liveUpdate;
        control.maximumUtf8Bytes = node.maximumUtf8Bytes;
        control.minimum = node.minimum;
        control.maximum = node.maximum;
        control.step = node.step;
        if (const auto action = node.events.find("change");
            action != node.events.end()) control.changeAction = action->second;
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
    if (IsScrollContainer(node.type) && node.clipFrame)
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    for (const auto& child : node.children)
        if (!CollectInputs(child, controls, childClip, error)) return false;
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
    if (!node.visible) return true;
    std::optional<ViewRect> childClip = inheritedClip;
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
        if (IsVirtualCollection(node.type))
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
                        child.margin - clip.y :
                    child.frame.x + child.frame.width + child.margin - clip.x);
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
        if (IsVirtualCollection(node.type))
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
        else
        {
            TranslateTree(node.children.front(),
                vertical ? 0.0f : -offset,
                vertical ? -offset : 0.0f);
        }

        if (inheritedClip && !Overlaps(clip, *inheritedClip))
            return true;
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
    LayoutNode(root, { 0.0f, 0.0f, width, height });
    return true;
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
    return true;
}

bool CollectViewInputControls(const ViewNode& root,
    std::vector<ViewInputControl>& controls, std::string& error)
{
    error.clear();
    controls.clear();
    return CollectInputs(root, controls, std::nullopt, error);
}

bool ApplyViewScrollOffsets(ViewNode& root,
    const ViewScrollOffsetResolver& resolver,
    std::vector<ViewScrollViewport>& viewports, std::string& error)
{
    error.clear();
    viewports.clear();
    std::size_t scrollContainers = 0;
    return ApplyScrollState(root, resolver, viewports, std::nullopt,
        scrollContainers, error);
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
