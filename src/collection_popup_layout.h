#pragma once

#include "constants.h"
#include "font_cu_rules.h"
#include "list_detail_rules.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace snowdesktop::collection_popup_layout
{

inline constexpr int kMaximumColumns = 5;
inline constexpr int kEmptyColumns = 3;
inline constexpr int kEmptyRows = 2;

struct Metrics
{
    float scale = 1.0f;
    int cellWidth = kCellWidth;
    int cellHeight = kMinCellHeight;
    int minimumListHeight = 1;
    int paddingX = kCollectionPopupPaddingX;
    int headerHeight = kCollectionPopupHeaderHeight;
    int bottomPadding = kCollectionPopupBottomPadding;
    int gapX = kCollectionPopupGapX;
    int gapY = kCollectionPopupGapY;
    int edgeMargin = 12;
    int anchorGap = 12;
    int maximumWidth = 560;
};

struct HeaderVerticalBounds
{
    int titleTop = 18;
    int titleBottom = 44;
    int sortButtonTop = 14;
    int sortButtonBottom = 48;
    int sortLabelOffsetY = 2;
};

inline int ScaleDimension(int value, float scale)
{
    if (!std::isfinite(scale))
        scale = 1.0f;
    return std::max(1, static_cast<int>(std::round(
        static_cast<float>(value) * std::clamp(scale, 0.1f, 8.0f))));
}

inline HeaderVerticalBounds ResolveHeaderVerticalBounds(float scale)
{
    HeaderVerticalBounds result;
    result.titleTop = ScaleDimension(18, scale);
    result.titleBottom = ScaleDimension(44, scale);

    const int sortButtonHeight = ScaleDimension(34, scale);
    result.sortButtonTop = result.titleTop +
        (result.titleBottom - result.titleTop - sortButtonHeight) / 2;
    result.sortButtonBottom = result.sortButtonTop + sortButtonHeight;
    result.sortLabelOffsetY = ScaleDimension(2, scale);
    return result;
}

/**
 * @brief Resolve popup geometry from the visual metrics of its owning page.
 *
 * Page cells can become smaller than the icon-and-title visual block when old
 * layout data or custom spacing is restored. Popup cells must preserve at
 * least that complete block, while enlarged pages must not be capped back to
 * the 92x116 baseline.
 */
inline Metrics ResolveMetrics(
    int pageCellWidth,
    int pageCellHeight,
    int minimumVisualWidth,
    int minimumVisualHeight,
    float pageScale,
    int minimumListHeight = 1)
{
    Metrics result;
    result.scale = std::isfinite(pageScale)
        ? std::clamp(pageScale, 0.1f, 8.0f)
        : 1.0f;
    result.cellWidth = std::max({
        1, pageCellWidth, minimumVisualWidth });
    result.cellHeight = std::max({
        1, pageCellHeight, minimumVisualHeight });
    result.minimumListHeight = std::max(1, minimumListHeight);
    result.paddingX = ScaleDimension(
        kCollectionPopupPaddingX, result.scale);
    result.headerHeight = ScaleDimension(
        kCollectionPopupHeaderHeight, result.scale);
    result.bottomPadding = ScaleDimension(
        kCollectionPopupBottomPadding, result.scale);
    result.gapX = ScaleDimension(
        kCollectionPopupGapX, result.scale);
    result.gapY = ScaleDimension(
        kCollectionPopupGapY, result.scale);
    result.edgeMargin = ScaleDimension(12, result.scale);
    result.anchorGap = ScaleDimension(12, result.scale);
    result.maximumWidth = ScaleDimension(560, result.scale);
    return result;
}

inline bool DetailsVisible(
    bool listMode,
    bool showModified,
    bool showType,
    bool showSize)
{
    return listMode && list_detail_rules::HasMetadataColumns(
        showModified, showType, showSize);
}

inline int ResolveListRowHeight(
    const Metrics& metrics,
    float listItemFontSizeCu)
{
    const float currentFont = font_cu_rules::Scale(
        listItemFontSizeCu, metrics.scale);
    const float defaultFont = font_cu_rules::Scale(
        kItemFontSize, metrics.scale);
    return std::max(
        metrics.minimumListHeight,
        list_detail_rules::RowHeight(
            ScaleDimension(36, metrics.scale),
            ScaleDimension(38, metrics.scale),
            currentFont,
            defaultFont));
}

inline int ResolveDetailsHeaderHeight(const Metrics& metrics)
{
    return ScaleDimension(30, metrics.scale);
}

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
