#pragma once

#include <algorithm>
#include <cstddef>

namespace snowdesktop::collection_popup_layout
{

inline constexpr int kMaximumColumns = 5;
inline constexpr int kEmptyColumns = 3;
inline constexpr int kEmptyRows = 2;

inline int PreferredColumnCount(
    std::size_t itemCount, int maximumColumns)
{
    maximumColumns = std::max(1, maximumColumns);
    if (itemCount == 0)
        return std::min(
            kEmptyColumns, maximumColumns);
    return std::clamp(
        static_cast<int>(std::min<std::size_t>(
            itemCount, kMaximumColumns)),
        1, maximumColumns);
}

inline int RequiredRowCount(
    std::size_t itemCount, int columns)
{
    columns = std::max(1, columns);
    if (itemCount == 0)
        return kEmptyRows;
    return std::max(
        1, (static_cast<int>(itemCount) +
               columns - 1) /
            columns);
}

/**
 * @brief 判断弹窗中的按下位置能否作为框选起点。
 *
 * 弹窗留白、标题栏和圆角内边缘都属于可用起点；条目和按钮等交互控件
 * 保留自身操作，不启动框选。
 */
inline constexpr bool AllowsMarqueeStart(
    bool pointInsidePopup,
    bool pointOnItem,
    bool pointOnControl)
{
    return pointInsidePopup &&
        !pointOnItem &&
        !pointOnControl;
}

} // namespace snowdesktop::collection_popup_layout
