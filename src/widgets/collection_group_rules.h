/**
 * @file collection_group_rules.h
 * @brief 集合组中不依赖窗口与渲染设备的可测试规则。
 *
 * 这里的函数同时供运行时代码和单元测试使用。新增同类滚动分类组件时，
 * 应优先复用或扩展这些规则，避免在鼠标、绘制和数据层分别复制算法。
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace snowdesktop::collection_group_rules
{

struct Rect
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

constexpr bool Intersects(const Rect& a, const Rect& b)
{
    return a.left < b.right && b.left < a.right &&
        a.top < b.bottom && b.top < a.bottom;
}

/**
 * @brief 分组子组件隐藏在组内时不再占用桌面网格。
 *
 * 所有落点预览、碰撞判断和实际落地必须共用该规则，避免出现预览为
 * 红色但仍可成功放置的“幽灵占位”。
 */
constexpr bool ShouldOccupyDesktopGrid(
    bool isGroupedChild)
{
    return !isGroupedChild;
}

/**
 * @brief 将命中矩形裁剪到可交互视口，完全不可见时返回空。
 */
constexpr std::optional<Rect> ClipToViewport(
    const Rect& candidate, const Rect& viewport)
{
    Rect clipped{
        std::max(candidate.left, viewport.left),
        std::max(candidate.top, viewport.top),
        std::min(candidate.right, viewport.right),
        std::min(candidate.bottom, viewport.bottom)
    };
    if (clipped.left >= clipped.right ||
        clipped.top >= clipped.bottom)
        return std::nullopt;
    return clipped;
}

/**
 * @brief 标签总宽不足视口时均匀铺满，余数从左到右分配。
 */
inline std::vector<int> DistributeWidthsToFill(
    std::vector<int> widths,
    int availableWidth)
{
    const int totalWidth = std::accumulate(
        widths.begin(), widths.end(), 0);
    if (widths.empty() ||
        totalWidth >= availableWidth)
        return widths;

    const int extra = availableWidth - totalWidth;
    const int perTab =
        extra / static_cast<int>(widths.size());
    const int remainder =
        extra % static_cast<int>(widths.size());
    for (size_t i = 0; i < widths.size(); ++i)
        widths[i] += perTab +
            (static_cast<int>(i) < remainder
                ? 1
                : 0);
    return widths;
}

/**
 * @brief 判断视口坐标中的条目是否命中内容坐标中的框选矩形。
 */
constexpr bool MarqueeSelectsViewportItem(
    Rect itemViewportRect, int scrollOffset,
    const Rect& marqueeContentRect)
{
    itemViewportRect.top += scrollOffset;
    itemViewportRect.bottom += scrollOffset;
    return Intersects(itemViewportRect, marqueeContentRect);
}

/**
 * @brief 返回仍然有效的活动标签；失效时稳定回退到第一个标签。
 */
template <typename T>
T ResolveActiveItem(
    const std::vector<T>& validItems,
    const T& requested)
{
    if (std::find(
            validItems.begin(), validItems.end(),
            requested) != validItems.end())
        return requested;
    return validItems.empty()
        ? T{}
        : validItems.front();
}

/**
 * @brief 按稳定顺序移动若干索引，并以原列表边界为插入位置。
 */
template <typename T>
std::vector<T> ReorderItems(
    const std::vector<T>& source,
    std::vector<size_t> selectedIndices,
    size_t insertBefore)
{
    std::erase_if(selectedIndices,
        [&](size_t index) {
            return index >= source.size();
        });
    std::sort(
        selectedIndices.begin(), selectedIndices.end());
    selectedIndices.erase(
        std::unique(
            selectedIndices.begin(),
            selectedIndices.end()),
        selectedIndices.end());
    if (selectedIndices.empty()) return source;

    std::vector<T> moving;
    std::vector<T> remaining;
    moving.reserve(selectedIndices.size());
    remaining.reserve(
        source.size() - selectedIndices.size());

    size_t selectedCursor = 0;
    for (size_t i = 0; i < source.size(); ++i)
    {
        if (selectedCursor < selectedIndices.size() &&
            selectedIndices[selectedCursor] == i)
        {
            moving.push_back(source[i]);
            ++selectedCursor;
        }
        else
        {
            remaining.push_back(source[i]);
        }
    }

    insertBefore = std::min(
        insertBefore, source.size());
    const size_t removedBefore =
        static_cast<size_t>(std::count_if(
            selectedIndices.begin(),
            selectedIndices.end(),
            [&](size_t index) {
                return index < insertBefore;
            }));
    const size_t adjustedInsert =
        std::min(insertBefore - removedBefore,
            remaining.size());
    remaining.insert(
        remaining.begin() +
            static_cast<std::ptrdiff_t>(
                adjustedInsert),
        moving.begin(), moving.end());
    return remaining;
}

enum class CollectionLabelDropSurface
{
    Desktop,
    CollectionGroup,
    FileGroup,
    FileList,
    Other,
};

constexpr bool AcceptsCollectionLabelDrop(
    CollectionLabelDropSurface surface)
{
    return surface ==
            CollectionLabelDropSurface::Desktop ||
        surface ==
            CollectionLabelDropSurface::CollectionGroup;
}

enum class GroupedDragKind
{
    CollectionLabel,
    FileGroupLabel,
    FileEntry,
};

constexpr bool AcceptsGroupedDrag(
    GroupedDragKind drag,
    CollectionLabelDropSurface surface)
{
    switch (drag)
    {
    case GroupedDragKind::CollectionLabel:
        return surface ==
                CollectionLabelDropSurface::Desktop ||
            surface ==
                CollectionLabelDropSurface::CollectionGroup;
    case GroupedDragKind::FileGroupLabel:
        return surface ==
                CollectionLabelDropSurface::Desktop ||
            surface ==
                CollectionLabelDropSurface::FileGroup;
    case GroupedDragKind::FileEntry:
        return surface ==
                CollectionLabelDropSurface::Desktop ||
            surface ==
                CollectionLabelDropSurface::FileList;
    }
    return false;
}

enum class FileGroupChildKind
{
    DesktopFileCategories,
    FolderMapping,
    Collection,
    CollectionGroup,
    FileGroup,
    Lua,
    Guide,
};

constexpr bool AcceptsFileGroupChild(
    FileGroupChildKind kind)
{
    return kind ==
            FileGroupChildKind::DesktopFileCategories ||
        kind == FileGroupChildKind::FolderMapping;
}

constexpr bool ShouldShowInnerCategoryTabs(
    bool enabled, bool searchBoxVisible,
    bool searchTextEmpty)
{
    return enabled &&
        (!searchBoxVisible || searchTextEmpty);
}

constexpr bool ShouldShowFileGroupSourceTabs(
    bool searchBoxVisible, bool searchTextEmpty)
{
    return !searchBoxVisible || searchTextEmpty;
}

constexpr int ClampIndependentTabScroll(
    int requested, int contentWidth,
    int viewportWidth)
{
    return std::clamp(
        requested, 0,
        std::max(0, contentWidth - viewportWidth));
}

/**
 * @brief 计算分类组件内容区顶部，依次避让可见标签、搜索框和宿主标签行。
 *
 * 标签矩形已包含搜索框与宿主行偏移，因此可见时直接以标签底部为准。
 * 标签隐藏时，仍需从搜索框底部继续预留宿主标签行，避免内容与外层
 * 文件组标签重叠。
 */
constexpr int ResolveCategorizedContentTop(
    int bodyTop, int bodyBottom,
    bool tabsVisible, int tabsBottom,
    bool searchVisible, int searchBottom,
    int hostedTabRows, int tabRowStride,
    int tabsGap, int searchGap)
{
    int contentTop = bodyTop;
    if (tabsVisible)
        contentTop = tabsBottom + tabsGap;
    else if (searchVisible)
        contentTop = searchBottom + searchGap +
            std::max(0, hostedTabRows) *
                std::max(0, tabRowStride);
    else
        contentTop += std::max(0, hostedTabRows) *
            std::max(0, tabRowStride);
    return std::clamp(contentTop, bodyTop, bodyBottom);
}

enum class DragSourceSelection
{
    Captured,
    Rebuild,
};

/**
 * @brief 选择提交拖放时的来源。
 *
 * 悬停切换标签只改变目标视图；只要仍是同一拖拽会话，就必须使用开始
 * 拖动时捕获的来源，不得从当前标签重新推导。
 */
constexpr DragSourceSelection SelectDragSource(
    bool capturedAvailable, bool sameDragSession)
{
    return capturedAvailable && sameDragSession
        ? DragSourceSelection::Captured
        : DragSourceSelection::Rebuild;
}

template <typename T, typename IsAllowed>
std::vector<T> ClaimUniqueAllowedItems(
    const std::vector<T>& candidates,
    std::vector<T>& claimed,
    IsAllowed&& isAllowed)
{
    std::vector<T> result;
    for (const auto& item : candidates)
    {
        if (!isAllowed(item) ||
            std::find(claimed.begin(),
                claimed.end(), item) !=
                claimed.end() ||
            std::find(result.begin(),
                result.end(), item) !=
                result.end())
            continue;
        result.push_back(item);
        claimed.push_back(item);
    }
    return result;
}

/**
 * @brief 取出分组的全部子项并清空活动项。
 *
 * 删除分组时使用该规则，返回值保留每个子项携带的原始尺寸等元数据，
 * 供调用方逐个安排桌面落点。
 */
template <typename T, typename Active>
std::vector<T> TakeAllForRelease(
    std::vector<T>& children, Active& active)
{
    std::vector<T> released = std::move(children);
    children.clear();
    active = Active{};
    return released;
}

struct Span
{
    int columns = 1;
    int rows = 1;
};

struct Placement
{
    int column = 0;
    int row = 0;
    Span span;
};

/**
 * @brief 为集合标签拖出规划精确桌面落点。
 *
 * 不搜索替代位置：任一目标区域被占用时整体失败，以保证蓝/红占位提示
 * 与最终落点一致。
 */
template <typename IsAreaOccupied, typename MarkAreaOccupied>
std::optional<std::vector<Placement>>
PlanExactPlacements(
    int pageColumns, int pageRows,
    int targetColumn, int targetRow,
    const std::vector<Span>& requestedSpans,
    IsAreaOccupied&& isAreaOccupied,
    MarkAreaOccupied&& markAreaOccupied)
{
    pageColumns = std::max(1, pageColumns);
    pageRows = std::max(1, pageRows);

    std::vector<Placement> result;
    result.reserve(requestedSpans.size());
    int cursorColumn = targetColumn;
    int cursorRow = targetRow;

    for (const Span& requested : requestedSpans)
    {
        Placement placement;
        placement.span.columns = std::clamp(
            requested.columns, 1, pageColumns);
        placement.span.rows = std::clamp(
            requested.rows, 1, pageRows);
        placement.column = std::clamp(
            cursorColumn, 0,
            pageColumns - placement.span.columns);
        placement.row = std::clamp(
            cursorRow, 0,
            pageRows - placement.span.rows);

        if (isAreaOccupied(placement))
            return std::nullopt;

        result.push_back(placement);
        markAreaOccupied(placement);

        cursorColumn =
            placement.column +
            placement.span.columns;
        cursorRow = placement.row;
        if (cursorColumn >= pageColumns)
        {
            cursorColumn = 0;
            cursorRow += placement.span.rows;
        }
    }
    return result;
}

}
