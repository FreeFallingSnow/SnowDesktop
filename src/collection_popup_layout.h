#pragma once

#include "constants.h"
#include "font_cu_rules.h"
#include "list_detail_rules.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace snowdesktop::collection_popup_layout
{

inline constexpr int kMaximumColumns = 5;
inline constexpr int kEmptyColumns = 3;
inline constexpr int kEmptyRows = 2;
inline constexpr int kMinimumListRows = 5;

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
    int maximumHeight = 640;
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

// Rendering and inline title editing must share the visible popup anchor.
inline RECT ResolveTitleRect(const RECT& popup, float scale)
{
    const auto header = ResolveHeaderVerticalBounds(scale);
    const int inset = ScaleDimension(22, scale);
    return { popup.left + inset, popup.top + header.titleTop,
        popup.right - inset, popup.top + header.titleBottom };
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
    result.maximumHeight = ScaleDimension(640, result.scale);
    return result;
}

inline int ResolveMaximumHeight(
    const Metrics& metrics,
    int workHeight)
{
    const int availableHeight = std::max(
        1, workHeight - metrics.edgeMargin * 2);
    return std::min(
        availableHeight,
        std::max(1, metrics.maximumHeight));
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
    const int minimumColumns = std::min(
        kEmptyColumns, maximumColumns);
    if (itemCount == 0)
        return minimumColumns;
    return std::clamp(
        std::max(
            minimumColumns,
            static_cast<int>(std::min<std::size_t>(
                itemCount, kMaximumColumns))),
        minimumColumns, maximumColumns);
}

inline int RequiredRowCount(
    std::size_t itemCount, int columns)
{
    columns = std::max(1, columns);
    if (itemCount == 0)
        return kEmptyRows;
    return std::max(
        kEmptyRows, (static_cast<int>(itemCount) +
               columns - 1) /
            columns);
}

inline int RequiredListRowCount(std::size_t itemCount)
{
    return std::max(
        kMinimumListRows,
        static_cast<int>(itemCount));
}

// Fan rows share a single vertical scroll coordinate with hit testing,
// keyboard navigation and drag insertion. Curvature depends on the visible
// position, so scrolling moves items along the arc instead of off its side.
inline int FanRowHeight(const Metrics& metrics)
{
    return std::max(ScaleDimension(64, metrics.scale),
        metrics.minimumListHeight + ScaleDimension(12, metrics.scale));
}

inline int FanContentHeight(const Metrics& metrics, std::size_t count)
{
    const auto pitch = static_cast<std::size_t>(FanRowHeight(metrics));
    return static_cast<int>(std::min(count,
        static_cast<std::size_t>(std::numeric_limits<int>::max() / 4) / pitch) * pitch);
}

inline RECT FanItemRect(const Metrics& metrics, const RECT& content,
    std::size_t index, int scrollOffset, bool rootAbove, bool bendLeft)
{
    const int pitch = FanRowHeight(metrics);
    const int width = std::max(1L, content.right - content.left);
    const int height = std::max(1L, content.bottom - content.top);
    const int bend = std::min(ScaleDimension(64, metrics.scale), width / 4);
    const int top = content.top + FanContentHeight(metrics, index) - scrollOffset;
    double t = std::clamp((top - content.top + pitch * 0.5) / height, 0.0, 1.0);
    if (!rootAbove) t = 1.0 - t;
    int shift = static_cast<int>(std::lround(bend * t * t));
    if (bendLeft) shift = bend - shift;
    const int gutter = std::min(ScaleDimension(10, metrics.scale), width / 8);
    return { content.left + shift, top,
        content.left + shift + std::max(1, width - bend - gutter), top + pitch };
}

inline RECT FanIconRect(const Metrics& metrics, const RECT& row)
{
    const int inset = ScaleDimension(5, metrics.scale);
    const int size = std::max(1, std::min({ScaleDimension(48, metrics.scale),
        static_cast<int>(row.bottom - row.top) - inset * 2,
        static_cast<int>(row.right - row.left) / 3}));
    const int top = row.top + (row.bottom - row.top - size) / 2;
    return {row.left + inset, top, row.left + inset + size, top + size};
}

inline RECT FanTextRect(const Metrics& metrics, const RECT& row)
{
    RECT text = row;
    text.left = FanIconRect(metrics, row).right + ScaleDimension(10, metrics.scale);
    text.right = std::max(text.left + 1,
        row.right - ScaleDimension(8, metrics.scale));
    return text;
}

inline float FanRevealProgress(float progress, float distanceFromRoot)
{
    // Using the same pose for both directions preserves continuity on reversal.
    const float delay = 0.22f * std::clamp(distanceFromRoot, 0.0f, 1.0f);
    const float t = std::clamp((progress - delay) / (1.0f - delay), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
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
