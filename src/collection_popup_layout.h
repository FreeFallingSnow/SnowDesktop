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

// A compact fan reserves its last slot for "Show all". File indices move along
// a fixed curve; painting and hit testing share its fractional slot positions.
inline int FanRowHeight(const Metrics& metrics)
{
    return std::max(ScaleDimension(72, metrics.scale),
        metrics.minimumListHeight + ScaleDimension(24, metrics.scale));
}

inline int FanFrameHeight(const Metrics& metrics, std::size_t slots)
{
    const auto pitch = static_cast<std::size_t>(FanRowHeight(metrics));
    return ScaleDimension(128, metrics.scale) + static_cast<int>(std::min(
        slots > 0 ? slots - 1 : 0,
        static_cast<std::size_t>(std::numeric_limits<int>::max() / 4) / pitch) * pitch);
}

inline std::size_t FanSlotCapacity(const Metrics& metrics, int height)
{
    return height < FanFrameHeight(metrics, 1) ? 0 :
        1 + static_cast<std::size_t>((height - FanFrameHeight(metrics, 1)) / FanRowHeight(metrics));
}

inline std::size_t FanVisibleItemCount(const Metrics& metrics, int height, std::size_t count)
{
    const auto slots = FanSlotCapacity(metrics, height);
    return std::min(count, slots > 0 ? slots - 1 : 0);
}

struct FanPoint { float x = 0; float y = 0; };
struct FanItem
{
    RECT icon{};
    RECT label{};
    FanPoint center{};
    float angle = 0;
};

inline FanPoint RotateFanPoint(FanPoint point, FanPoint center, float angle)
{
    const double radians = angle * 3.14159265358979323846 / 180.0;
    const double c = std::cos(radians), s = std::sin(radians);
    const double x = point.x - center.x, y = point.y - center.y;
    return {static_cast<float>(center.x + x * c - y * s),
        static_cast<float>(center.y + x * s + y * c)};
}

inline RECT FanRotatedBounds(const RECT& rect, const FanItem& item)
{
    float left = std::numeric_limits<float>::max(), top = left;
    float right = -left, bottom = -left;
    for (const auto x : {rect.left, rect.right})
    for (const auto y : {rect.top, rect.bottom})
    {
        const auto point = RotateFanPoint(
            {static_cast<float>(x), static_cast<float>(y)}, item.center, item.angle);
        left = std::min(left, point.x); right = std::max(right, point.x);
        top = std::min(top, point.y); bottom = std::max(bottom, point.y);
    }
    return {static_cast<LONG>(std::floor(left)), static_cast<LONG>(std::floor(top)),
        static_cast<LONG>(std::ceil(right)), static_cast<LONG>(std::ceil(bottom))};
}

inline RECT FanItemBounds(const FanItem& item)
{
    const auto icon = FanRotatedBounds(item.icon, item);
    const auto label = FanRotatedBounds(item.label, item);
    return {std::min(icon.left, label.left), std::min(icon.top, label.top),
        std::max(icon.right, label.right), std::max(icon.bottom, label.bottom)};
}

inline bool FanRectContains(const FanItem& item, const RECT& rect, POINT point)
{
    const auto local = RotateFanPoint(
        {static_cast<float>(point.x), static_cast<float>(point.y)}, item.center, -item.angle);
    return local.x >= rect.left && local.x < rect.right &&
        local.y >= rect.top && local.y < rect.bottom;
}

inline bool FanItemContains(const FanItem& item, POINT point)
{
    return FanRectContains(item, item.icon, point) || FanRectContains(item, item.label, point);
}

inline RECT FanHandoffBounds(const FanItem& item, const Metrics& metrics)
{
    const int pad = ScaleDimension(4, metrics.scale);
    return {item.icon.left - pad, item.icon.top - pad,
        item.icon.right + pad, item.icon.bottom + pad};
}

struct FanLine { FanPoint start, end; };
inline FanLine FanInsertionLine(const FanItem& item, const Metrics& metrics,
    bool rootAbove, bool after)
{
    const float y = item.center.y + FanRowHeight(metrics) * 0.5f *
        (rootAbove ? 1.0f : -1.0f) * (after ? 1.0f : -1.0f);
    return {RotateFanPoint({static_cast<float>(std::min(item.icon.left, item.label.left)), y},
                item.center, item.angle),
        RotateFanPoint({static_cast<float>(std::max(item.icon.right, item.label.right)), y},
                item.center, item.angle)};
}

inline bool FanIsAfterInsertion(const FanItem& item, POINT point, bool rootAbove)
{
    const auto local = RotateFanPoint({static_cast<float>(point.x), static_cast<float>(point.y)},
        item.center, -item.angle);
    return rootAbove ? local.y >= item.center.y : local.y < item.center.y;
}

inline long long FanInsertionDistanceSquared(const FanLine& line, POINT point)
{
    const double dx = line.end.x - line.start.x, dy = line.end.y - line.start.y;
    const double length = dx * dx + dy * dy;
    const double t = length > 0 ? std::clamp(
        ((point.x - line.start.x) * dx + (point.y - line.start.y) * dy) / length, 0.0, 1.0) : 0;
    const double x = point.x - (line.start.x + t * dx), y = point.y - (line.start.y + t * dy);
    return static_cast<long long>(std::llround(x * x + y * y));
}

inline int FanRootX(const Metrics& metrics, int width, bool mirrored)
{
    const int inset = ScaleDimension(110, metrics.scale);
    return mirrored ? inset : width - inset;
}

inline FanItem ResolveFanItem(const Metrics& metrics, const RECT& popup,
    double position, bool rootAbove, bool mirrored)
{
    const float t = std::clamp(static_cast<float>(position) / 7.0f, 0.0f, 1.0f);
    const int bend = static_cast<int>(std::round(ScaleDimension(64, metrics.scale) * t * t));
    const int halfIcon = ScaleDimension(28, metrics.scale);
    const int x = popup.left + FanRootX(metrics, popup.right - popup.left, mirrored) +
        (mirrored ? -bend : bend);
    const int fromRoot = ScaleDimension(46, metrics.scale) +
        static_cast<int>(std::round(position * FanRowHeight(metrics)));
    const int y = rootAbove ? popup.top + fromRoot : popup.bottom - fromRoot;
    FanItem result;
    result.center = {static_cast<float>(x), static_cast<float>(y)};
    result.angle = 14.0f * t * (mirrored ? -1.0f : 1.0f) * (rootAbove ? -1.0f : 1.0f);
    result.icon = {x - halfIcon, y - halfIcon, x + halfIcon, y + halfIcon};
    const int gap = ScaleDimension(10, metrics.scale);
    const int width = ScaleDimension(230, metrics.scale);
    const int halfLabel = ScaleDimension(16, metrics.scale);
    result.label = mirrored
        ? RECT{result.icon.right + gap, y - halfLabel,
            result.icon.right + gap + width, y + halfLabel}
        : RECT{result.icon.left - gap - width, y - halfLabel,
            result.icon.left - gap, y + halfLabel};
    return result;
}

inline double FanMaximumScroll(std::size_t count, std::size_t visible)
{
    return static_cast<double>(count > visible ? count - visible : 0);
}

inline float FanItemOpacity(double position, std::size_t visible)
{
    if (!visible) return 0;
    // Fade inside the small end margins instead of clipping a fully opaque icon.
    constexpr double edge = 0.25;
    return static_cast<float>(std::clamp(std::min((position + edge) / edge,
        (static_cast<double>(visible) - 1.0 + edge - position) / edge), 0.0, 1.0));
}

struct FanRange { std::size_t first = 0, end = 0; };
inline FanRange FanVisibleRange(double offset, std::size_t visible, std::size_t count)
{
    offset = std::clamp(offset, 0.0, FanMaximumScroll(count, visible));
    return {static_cast<std::size_t>(std::floor(offset)),
        std::min(count, static_cast<std::size_t>(std::ceil(offset)) + visible)};
}

inline FanPoint FanUnfoldCenter(FanPoint origin, FanPoint destination, float progress)
{
    const float t = std::clamp(progress, 0.0f, 1.0f);
    // Rise out of the stack before bending sideways. Icon size stays constant.
    return {origin.x + (destination.x - origin.x) * t * t,
        origin.y + (destination.y - origin.y) * t};
}

struct FanScrollState
{
    double position = 0, target = 0, from = 0;
    double started = 0, duration = 0;

    bool Advance(double now)
    {
        const double t = duration > 0 ? std::clamp((now - started) / duration, 0.0, 1.0) : 1.0;
        const double rest = 1.0 - t;
        position = from + (target - from) * (1.0 - rest * rest * rest);
        if (t >= 1.0) position = target;
        return t < 1.0 && from != target;
    }
    void Clamp(double maximum)
    {
        maximum = std::max(0.0, maximum);
        position = std::clamp(position, 0.0, maximum);
        from = std::clamp(from, 0.0, maximum);
        target = std::clamp(target, 0.0, maximum);
    }
    void MoveTo(double value, double maximum, double now, double milliseconds)
    {
        Advance(now);
        Clamp(maximum);
        value = std::clamp(value, 0.0, std::max(0.0, maximum));
        if (value == target && milliseconds > 0) return;
        from = position;
        target = value;
        started = now;
        duration = milliseconds;
        if (duration <= 0) position = from = target;
    }
};

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
