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
    const std::optional<float>& savedListSize,
    float iconTitleSize)
{
    return savedListSize && *savedListSize >= 10.0f &&
            *savedListSize <= 24.0f
        ? *savedListSize
        : iconTitleSize;
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

inline Columns BuildColumns(
    int availableWidth,
    int threshold300,
    int threshold390,
    int threshold510,
    int modifiedWidth,
    int typeWidth,
    int sizeWidth)
{
    Columns result;
    result.showModified = availableWidth >= threshold300;
    result.showSize = availableWidth >= threshold390;
    result.showType = availableWidth >= threshold510;
    result.modifiedWidth = result.showModified ? modifiedWidth : 0;
    result.typeWidth = result.showType ? typeWidth : 0;
    result.sizeWidth = result.showSize ? sizeWidth : 0;
    result.nameWidth = std::max(
        1,
        availableWidth - result.modifiedWidth -
            result.typeWidth - result.sizeWidth);
    return result;
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
