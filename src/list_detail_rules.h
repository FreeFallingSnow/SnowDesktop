#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace snowdesktop::list_detail_rules
{

enum class Column
{
    None,
    Name,
    Modified,
    Type,
    Size,
};

inline constexpr std::string_view ToString(Column column)
{
    switch (column)
    {
    case Column::Name: return "name";
    case Column::Modified: return "modified";
    case Column::Type: return "type";
    case Column::Size: return "size";
    default: return "none";
    }
}

inline constexpr Column FromString(std::string_view value)
{
    if (value == "name") return Column::Name;
    if (value == "modified") return Column::Modified;
    if (value == "type") return Column::Type;
    if (value == "size") return Column::Size;
    return Column::None;
}

inline constexpr bool DefaultAscending(Column column)
{
    return column == Column::Name || column == Column::Type;
}

inline constexpr Column FromLegacyFolderSortMode(int mode)
{
    switch (mode)
    {
    case 0: return Column::Name;
    case 1: return Column::Type;
    case 2: return Column::Modified;
    case 3: return Column::Size;
    default: return Column::None;
    }
}

inline float ResolveFontSize(
    const std::optional<float>& savedListSizeCu,
    float iconTitleSizeCu)
{
    return savedListSizeCu && *savedListSizeCu >= 10.0f &&
            *savedListSizeCu <= 24.0f
        ? *savedListSizeCu
        : iconTitleSizeCu;
}

inline int RowHeight(
    int scaled36,
    int scaled38,
    float currentFontPixels,
    float defaultFontPixels)
{
    const float lineDelta =
        (currentFontPixels - defaultFontPixels) * 7.0f / 6.0f;
    return std::max(
        scaled36,
        scaled38 + static_cast<int>(std::ceil(lineDelta)));
}

inline int HeaderHeight(
    int scaled28,
    int scaled10,
    float currentFontPixels)
{
    return std::max(
        scaled28,
        static_cast<int>(std::ceil(
            currentFontPixels * 7.0f / 6.0f)) + scaled10);
}

struct Columns
{
    int nameWidth = 0;
    int modifiedWidth = 0;
    int typeWidth = 0;
    int sizeWidth = 0;
    bool showModified = false;
    bool showType = false;
    bool showSize = false;
};

inline constexpr float kDefaultModifiedPosition = 140.0f / 510.0f;
inline constexpr float kDefaultTypePosition = 300.0f / 510.0f;
inline constexpr float kDefaultSizePosition = 420.0f / 510.0f;
inline constexpr float kMinimumDividerPosition = 0.05f;
inline constexpr float kMaximumDividerPosition = 0.95f;
inline constexpr float kMinimumDividerGap = 0.04f;

struct DividerPositions
{
    float modified = kDefaultModifiedPosition;
    float type = kDefaultTypePosition;
    float size = kDefaultSizePosition;
};

inline constexpr float ClampStoredPosition(float position)
{
    return std::clamp(position,
        kMinimumDividerPosition, kMaximumDividerPosition);
}

inline constexpr bool HasMetadataColumns(
    bool showModified, bool showType, bool showSize)
{
    return showModified || showType || showSize;
}

inline Columns BuildColumns(
    int availableWidth,
    bool showModified,
    bool showType,
    bool showSize,
    float modifiedPosition,
    float typePosition,
    float sizePosition)
{
    Columns result;
    availableWidth = std::max(1, availableWidth);
    result.showModified = showModified;
    result.showType = showType;
    result.showSize = showSize;

    struct VisibleDivider
    {
        Column column;
        int position;
    };
    VisibleDivider dividers[3]{};
    size_t count = 0;
    const auto append = [&](Column column, bool visible, float position) {
        if (!visible) return;
        const int pixel = static_cast<int>(std::lround(
            static_cast<float>(availableWidth) *
                ClampStoredPosition(position)));
        dividers[count++] = {
            column, std::clamp(pixel, 0, availableWidth) };
    };
    append(Column::Modified, showModified, modifiedPosition);
    append(Column::Type, showType, typePosition);
    append(Column::Size, showSize, sizePosition);

    if (count == 0)
    {
        result.nameWidth = availableWidth;
        return result;
    }

    result.nameWidth = dividers[0].position;
    for (size_t index = 0; index < count; ++index)
    {
        const int right = index + 1 < count
            ? dividers[index + 1].position
            : availableWidth;
        const int width = std::max(0,
            right - dividers[index].position);
        switch (dividers[index].column)
        {
        case Column::Modified:
            result.modifiedWidth = width;
            break;
        case Column::Type:
            result.typeWidth = width;
            break;
        case Column::Size:
            result.sizeWidth = width;
            break;
        default:
            break;
        }
    }
    return result;
}

inline DividerPositions NormalizePositions(
    bool showModified, bool showType, bool showSize,
    DividerPositions positions)
{
    positions.modified = ClampStoredPosition(positions.modified);
    positions.type = ClampStoredPosition(positions.type);
    positions.size = ClampStoredPosition(positions.size);

    const int afterModified = static_cast<int>(showType) +
        static_cast<int>(showSize);
    if (showModified)
    {
        positions.modified = std::clamp(
            positions.modified,
            kMinimumDividerPosition,
            kMaximumDividerPosition -
                kMinimumDividerGap * afterModified);
    }
    if (showType)
    {
        const float lower = showModified
            ? positions.modified + kMinimumDividerGap
            : kMinimumDividerPosition;
        const float upper = kMaximumDividerPosition -
            (showSize ? kMinimumDividerGap : 0.0f);
        positions.type = std::clamp(
            positions.type, lower, upper);
    }
    if (showSize)
    {
        const float lower = showType
            ? positions.type + kMinimumDividerGap
            : showModified
                ? positions.modified + kMinimumDividerGap
                : kMinimumDividerPosition;
        positions.size = std::clamp(
            positions.size, lower, kMaximumDividerPosition);
    }
    return positions;
}

inline float ClampDraggedPosition(
    Column column, float proposed,
    bool showModified, bool showType, bool showSize,
    const DividerPositions& positions)
{
    float lower = kMinimumDividerPosition;
    float upper = kMaximumDividerPosition;
    switch (column)
    {
    case Column::Modified:
        if (showType)
            upper = positions.type - kMinimumDividerGap;
        else if (showSize)
            upper = positions.size - kMinimumDividerGap;
        break;
    case Column::Type:
        if (showModified)
            lower = positions.modified + kMinimumDividerGap;
        if (showSize)
            upper = positions.size - kMinimumDividerGap;
        break;
    case Column::Size:
        if (showType)
            lower = positions.type + kMinimumDividerGap;
        else if (showModified)
            lower = positions.modified + kMinimumDividerGap;
        break;
    default:
        return ClampStoredPosition(proposed);
    }
    if (lower > upper) return (lower + upper) * 0.5f;
    return std::clamp(proposed, lower, upper);
}

inline DividerPositions LegacyWidthsToPositions(
    float modifiedWidth, float typeWidth, float sizeWidth,
    float baselineWidth = 510.0f)
{
    baselineWidth = std::max(1.0f, baselineWidth);
    DividerPositions positions;
    positions.modified = 1.0f -
        (modifiedWidth + typeWidth + sizeWidth) / baselineWidth;
    positions.type = 1.0f -
        (typeWidth + sizeWidth) / baselineWidth;
    positions.size = 1.0f - sizeWidth / baselineWidth;
    return NormalizePositions(true, true, true, positions);
}

inline Column HitDivider(
    const Columns& columns, int relativeX, int tolerance)
{
    int left = columns.nameWidth;
    const auto hit = [&](Column column, bool visible) {
        return visible && std::abs(relativeX - left) <= tolerance
            ? column : Column::None;
    };
    if (const Column column = hit(Column::Modified,
            columns.showModified); column != Column::None)
        return column;
    if (columns.showModified) left += columns.modifiedWidth;
    if (const Column column = hit(Column::Type,
            columns.showType); column != Column::None)
        return column;
    if (columns.showType) left += columns.typeWidth;
    if (const Column column = hit(Column::Size,
            columns.showSize); column != Column::None)
        return column;
    return Column::None;
}

inline Column HitColumn(const Columns& columns, int relativeX)
{
    int right = columns.nameWidth;
    if (relativeX < right) return Column::Name;
    if (columns.showModified)
    {
        right += columns.modifiedWidth;
        if (relativeX < right) return Column::Modified;
    }
    if (columns.showType)
    {
        right += columns.typeWidth;
        if (relativeX < right) return Column::Type;
    }
    return columns.showSize ? Column::Size : Column::Name;
}

} // namespace snowdesktop::list_detail_rules
