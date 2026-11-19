#pragma once

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

struct DividerPositions
{
    float modified = kDefaultModifiedPosition;
    float type = kDefaultTypePosition;
    float size = kDefaultSizePosition;
};

} // namespace snowdesktop::list_detail_rules
