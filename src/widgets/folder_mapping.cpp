/**
 * @file folder_mapping.cpp
 * @brief 映射文件夹组件实现
 *
 * 该组件用于在桌面网格中显示磁盘上的映射文件夹内容，
 * 支持图标模式与列表模式切换，以及打开文件夹按钮。
 * 提供文件夹条目在桌面网格中的布局、绘制、
 * 拖放排序和滚动等功能。
 */

#include "widget.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include "app.h"
#include "collection_group_rules.h"
#include "drop_model.h"
#include "search_match.h"
#include "../category_settings.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include <unordered_set>
#include "../l10n.h"

static RECT FolderMappingItemRect(FolderMapping* widget, size_t linearIndex);

static std::wstring FolderEntryCategoryId(
    const FolderEntry& entry, const CategorySettings& settings)
{
    if (entry.isDirectory)
        return L"folders";
    std::wstring categoryId =
        CategoryIdForExtension(settings, ToUpperInvariant(PathFindExtensionW(entry.name.c_str())));
    return categoryId.empty() ? L"others" : categoryId;
}

static const std::vector<std::wstring>& FolderEntryDateGroupOrder()
{
    static const std::vector<std::wstring> order = {
        L"today", L"yesterday", L"this_week", L"last_week",
        L"this_month", L"last_month", L"this_year", L"older",
    };
    return order;
}

static std::wstring FolderEntryDateGroupLabel(const std::wstring& id)
{
    if (id == L"today") return _LW("widget.categories.today");
    if (id == L"yesterday") return _LW("widget.categories.yesterday");
    if (id == L"this_week") return _LW("widget.categories.this_week");
    if (id == L"last_week") return _LW("widget.categories.last_week");
    if (id == L"this_month") return _LW("widget.categories.this_month");
    if (id == L"last_month") return _LW("widget.categories.last_month");
    if (id == L"this_year") return _LW("widget.categories.this_year");
    return _LW("widget.categories.earlier");
}

static int FolderEntryDayDiff(const SYSTEMTIME& newer, const SYSTEMTIME& older)
{
    FILETIME newerFileTime{}, olderFileTime{};
    SystemTimeToFileTime(&newer, &newerFileTime);
    SystemTimeToFileTime(&older, &olderFileTime);
    ULARGE_INTEGER newerValue{}, olderValue{};
    newerValue.LowPart = newerFileTime.dwLowDateTime;
    newerValue.HighPart = newerFileTime.dwHighDateTime;
    olderValue.LowPart = olderFileTime.dwLowDateTime;
    olderValue.HighPart = olderFileTime.dwHighDateTime;
    if (newerValue.QuadPart < olderValue.QuadPart)
        return -1;
    return static_cast<int>(
        (newerValue.QuadPart - olderValue.QuadPart) / 864000000000ULL);
}

static std::wstring FolderEntryDateGroupId(const FolderEntry& entry)
{
    FILETIME localFileTime{};
    SYSTEMTIME fileTime{}, nowTime{};
    if (!FileTimeToLocalFileTime(&entry.lastWriteTime, &localFileTime) ||
        !FileTimeToSystemTime(&localFileTime, &fileTime))
        return L"older";
    GetLocalTime(&nowTime);

    SYSTEMTIME todayStart = nowTime;
    todayStart.wHour = 0;
    todayStart.wMinute = 0;
    todayStart.wSecond = 0;
    todayStart.wMilliseconds = 0;
    SYSTEMTIME fileDayStart = fileTime;
    fileDayStart.wHour = 0;
    fileDayStart.wMinute = 0;
    fileDayStart.wSecond = 0;
    fileDayStart.wMilliseconds = 0;
    int daysFromToday = FolderEntryDayDiff(todayStart, fileDayStart);

    if (daysFromToday <= 0) return L"today";
    if (daysFromToday == 1) return L"yesterday";
    int todayDayOfWeek =
        nowTime.wDayOfWeek == 0 ? 7 : static_cast<int>(nowTime.wDayOfWeek);
    if (daysFromToday < todayDayOfWeek) return L"this_week";
    if (daysFromToday < todayDayOfWeek + 7) return L"last_week";
    if (fileTime.wYear == nowTime.wYear &&
        fileTime.wMonth == nowTime.wMonth)
        return L"this_month";

    int previousMonth = nowTime.wMonth == 1 ? 12 : nowTime.wMonth - 1;
    int previousMonthYear = nowTime.wMonth == 1
        ? nowTime.wYear - 1
        : nowTime.wYear;
    if (fileTime.wYear == previousMonthYear &&
        fileTime.wMonth == previousMonth)
        return L"last_month";
    if (fileTime.wYear == nowTime.wYear) return L"this_year";
    return L"older";
}

static size_t FolderEntryDateGroupRank(const FolderEntry& entry)
{
    const auto& order = FolderEntryDateGroupOrder();
    auto it = std::find(order.begin(), order.end(), FolderEntryDateGroupId(entry));
    return it == order.end()
        ? order.size()
        : static_cast<size_t>(std::distance(order.begin(), it));
}

RECT FolderMapping::GetSearchBoxRect() const
{
    return GetCategorizedSearchBoxRect(
        data_ && app_ && data_->showSearchBox);
}

static RECT FolderMappingTabsRect(FolderMapping* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    return widget
        ? widget->GetCategorizedTabsRect(
            data && data->showFileCategories)
        : RECT{};
}

/**
 * @brief 计算映射文件夹内容区域的矩形
 * @param widget FolderMapping 组件指针
 * @return 内容区域的 RECT，已向内缩进 4 像素（水平）和 8 像素（垂直）
 */
static RECT FolderMappingContentRect(FolderMapping* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -widget->Cu(4.0f), -widget->Cu(8.0f));
    if (IsRectEmptyRect(body)) return {};
    RECT tabs = FolderMappingTabsRect(widget);
    if (!IsRectEmptyRect(tabs))
        body.top = std::min<LONG>(body.bottom, tabs.bottom + widget->Cu(8.0f));
    else
    {
        RECT search = widget->GetSearchBoxRect();
        if (!IsRectEmptyRect(search))
            body.top = std::min<LONG>(
                body.bottom,
                search.bottom + widget->Cu(4.0f) +
                    widget->GetCategorizedTabRowOffset() *
                        widget->Cu(38.0f));
        else if (widget->GetCategorizedTabRowOffset() > 0)
            body.top = std::min<LONG>(
                body.bottom,
                body.top +
                    widget->GetCategorizedTabRowOffset() *
                        widget->Cu(38.0f));
    }
    return body;
}

void FolderMapping::EnsureCategorySnapshot() const
{
    if (!data_ || !app_) return;

    std::vector<std::wstring> currentPaths;
    currentPaths.reserve(data_->folderEntries.size());
    for (const auto& entry : data_->folderEntries)
        currentPaths.push_back(ToUpperInvariant(entry.fullPath));
    if (categorySnapshotValid_ && currentPaths == categorySnapshotPaths_)
        return;

    categorySnapshotPaths_ = std::move(currentPaths);
    entryIndicesByCategory_.clear();
    visibleCategoryIds_.clear();
    visibleEntryIndices_.clear();
    visibleEntriesCategory_.clear();
    visibleEntriesSearch_.clear();
    visibleEntriesDateHeaders_ = false;
    dateLayoutCache_.clear();
    dateLayoutSource_.clear();

    auto& allEntries = entryIndicesByCategory_[L"all"];
    allEntries.reserve(data_->folderEntries.size());
    for (size_t i = 0; i < data_->folderEntries.size(); ++i)
    {
        allEntries.push_back(i);
        entryIndicesByCategory_[FolderEntryCategoryId(
            data_->folderEntries[i], app_->GetCategorySettings())].push_back(i);
    }

    for (const auto& categoryId : GetCategoryOrder(app_->GetCategorySettings()))
    {
        auto it = entryIndicesByCategory_.find(categoryId);
        if (it != entryIndicesByCategory_.end() && !it->second.empty())
            visibleCategoryIds_.push_back(categoryId);
    }
    categorySnapshotValid_ = true;
}

void FolderMapping::InvalidateFilterCache()
{
    categorySnapshotValid_ = false;
    categorySnapshotPaths_.clear();
    entryIndicesByCategory_.clear();
    visibleCategoryIds_.clear();
    visibleEntryIndices_.clear();
    visibleEntriesCategory_.clear();
    visibleEntriesSearch_.clear();
    visibleEntriesDateHeaders_ = false;
    dateLayoutCache_.clear();
    dateLayoutSource_.clear();
    InvalidateSlots();
}

std::wstring FolderMapping::CachedActiveCategoryId() const
{
    if (!data_) return L"";
    EnsureCategorySnapshot();
    if (data_->showFileCategories &&
        std::find(visibleCategoryIds_.begin(), visibleCategoryIds_.end(),
            data_->activeCategoryId) != visibleCategoryIds_.end())
        return data_->activeCategoryId;
    return visibleCategoryIds_.empty() ? L"" : visibleCategoryIds_.front();
}

const std::vector<size_t>& FolderMapping::GetVisibleEntryIndices() const
{
    static const std::vector<size_t> empty;
    if (!data_ || !app_) return empty;
    EnsureCategorySnapshot();

    const std::wstring categoryId =
        SearchAllCategories() && IsSearchActive()
            ? L"all"
            : (data_->showFileCategories
                ? CachedActiveCategoryId()
                : L"all");
    const std::wstring query = data_->showSearchBox ? searchText_ : L"";
    if (categoryId == visibleEntriesCategory_ &&
        query == visibleEntriesSearch_ &&
        data_->dateHeaders == visibleEntriesDateHeaders_)
        return visibleEntryIndices_;

    visibleEntryIndices_.clear();
    visibleEntriesCategory_ = categoryId;
    visibleEntriesSearch_ = query;
    visibleEntriesDateHeaders_ = data_->dateHeaders;

    auto it = entryIndicesByCategory_.find(categoryId.empty() ? L"all" : categoryId);
    if (it == entryIndicesByCategory_.end())
        return visibleEntryIndices_;
    for (size_t entryIndex : it->second)
    {
        if (entryIndex >= data_->folderEntries.size()) continue;
        if (query.empty() ||
            NameMatchesQuery(data_->folderEntries[entryIndex].name, query))
            visibleEntryIndices_.push_back(entryIndex);
    }
    if (data_->dateHeaders && query.empty())
    {
        std::stable_sort(visibleEntryIndices_.begin(), visibleEntryIndices_.end(),
            [this](size_t lhs, size_t rhs)
            {
                if (lhs >= data_->folderEntries.size() ||
                    rhs >= data_->folderEntries.size())
                    return lhs < rhs;
                const FolderEntry& left = data_->folderEntries[lhs];
                const FolderEntry& right = data_->folderEntries[rhs];
                size_t leftRank = FolderEntryDateGroupRank(left);
                size_t rightRank = FolderEntryDateGroupRank(right);
                if (leftRank != rightRank) return leftRank < rightRank;
                return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
            });
    }
    return visibleEntryIndices_;
}

const std::vector<std::wstring>&
FolderMapping::GetVisibleCategoryIds() const
{
    static const std::vector<std::wstring> empty;
    if (!data_ || !app_) return empty;
    EnsureCategorySnapshot();
    return visibleCategoryIds_;
}

const CategorySettings& FolderMapping::GetCategorySettingsForDisplay() const
{
    return app_->GetCategorySettings();
}

static std::wstring FolderMappingTabDisplayText(
    FolderMapping* widget, const std::wstring& categoryId)
{
    if (!widget || !widget->GetApp()) return categoryId;
    widget->GetVisibleEntryIndices();
    DesktopWidget* data = widget->GetWidgetData();
    size_t count = 0;
    if (data)
    {
        const CategorySettings& settings =
            widget->GetCategorySettingsForDisplay();
        for (const auto& entry : data->folderEntries)
        {
            if (categoryId == L"all" ||
                FolderEntryCategoryId(entry, settings) == categoryId)
                ++count;
        }
    }
    return GetCategoryLabel(widget->GetCategorySettingsForDisplay(), categoryId) +
        L" " + std::to_wstring(count);
}

static std::vector<int> FolderMappingTabWidths(
    FolderMapping* widget, int availableWidth)
{
    if (!widget) return {};
    widget->GetVisibleEntryIndices();
    DesktopWidget* data = widget->GetWidgetData();
    if (!data) return {};

    const CategorySettings& settings =
        widget->GetCategorySettingsForDisplay();
    std::unordered_map<std::wstring, size_t> counts;
    counts[L"all"] = data->folderEntries.size();
    for (const auto& entry : data->folderEntries)
        ++counts[FolderEntryCategoryId(entry, settings)];

    const std::vector<std::wstring> order = GetCategoryOrder(settings);
    std::vector<std::wstring> labels;
    for (const auto& categoryId : order)
    {
        auto countIt = counts.find(categoryId);
        if (countIt == counts.end() || countIt->second == 0) continue;
        labels.push_back(
            GetCategoryLabel(settings, categoryId) +
            L" " +
            std::to_wstring(countIt->second));
    }
    return widget->BuildCategorizedTabWidths(
        labels, availableWidth);
}

static int FolderMappingTabTotalWidth(const std::vector<int>& widths)
{
    int total = 0;
    for (int width : widths)
        total += width;
    return total;
}

static RECT FolderMappingTabRect(FolderMapping* widget, size_t index)
{
    if (!widget) return {};
    widget->GetVisibleEntryIndices();
    DesktopWidget* data = widget->GetWidgetData();
    if (!data) return {};

    std::vector<std::wstring> categories;
    const CategorySettings& settings =
        widget->GetCategorySettingsForDisplay();
    std::unordered_set<std::wstring> present;
    present.insert(L"all");
    for (const auto& entry : data->folderEntries)
        present.insert(FolderEntryCategoryId(entry, settings));
    for (const auto& categoryId : GetCategoryOrder(settings))
        if (present.contains(categoryId))
            categories.push_back(categoryId);
    if (index >= categories.size()) return {};

    RECT tabsRect = FolderMappingTabsRect(widget);
    if (IsRectEmptyRect(tabsRect)) return {};
    std::vector<int> widths =
        FolderMappingTabWidths(widget, tabsRect.right - tabsRect.left);
    if (index >= widths.size()) return {};
    int maxScroll = std::max(0,
        FolderMappingTabTotalWidth(widths) -
        static_cast<int>(tabsRect.right - tabsRect.left));
    int startX = tabsRect.left -
        std::clamp(data->tabScrollOffset, 0, maxScroll);
    for (size_t i = 0; i < index; ++i)
        startX += widths[i];
    RECT rect = MakeRect(startX, tabsRect.top,
        startX + widths[index], tabsRect.bottom);
    InflateRect(&rect, -widget->Cu(2.0f), -widget->Cu(2.0f));
    const auto clipped =
        snowdesktop::collection_group_rules::
            ClipToViewport(
                {
                    rect.left, rect.top,
                    rect.right, rect.bottom
                },
                {
                    tabsRect.left, tabsRect.top,
                    tabsRect.right, tabsRect.bottom
                });
    return clipped
        ? MakeRect(
            clipped->left, clipped->top,
            clipped->right, clipped->bottom)
        : RECT{};
}

RECT FolderMapping::GetContentViewportRect() const
{
    return FolderMappingContentRect(const_cast<FolderMapping*>(this));
}

void FolderMapping::ApplyMarqueeSelection(const RECT& contentRect)
{
    if (!data_)
        return;

    for (auto& entry : data_->folderEntries)
        entry.selected = false;

    const auto& visibleEntries = GetVisibleEntryIndices();
    const int scroll = GetScrollOffset();
    for (size_t i = 0; i < visibleEntries.size(); ++i)
    {
        RECT itemRect = FolderMappingItemRect(this, i);
        OffsetRect(&itemRect, 0, scroll);
        size_t entryIndex = visibleEntries[i];
        if (entryIndex < data_->folderEntries.size())
            data_->folderEntries[entryIndex].selected =
                RectsIntersect(itemRect, contentRect);
    }
}

/**
 * @brief 获取映射文件夹图标模式下每个单元格的高度
 * @param widget FolderMapping 组件指针
 * @return 单元格高度（像素），失败时返回最小单元格高度 kMinCellHeight
 */
static int FolderMappingCellHeight(FolderMapping* widget)
{
    if (!widget || !widget->GetApp() || !widget->GetApp()->GetDesktopGrid())
        return kMinCellHeight;
    DesktopWidget* data = widget->GetWidgetData();
    for (const auto& page : widget->GetApp()->GetDesktopGrid()->GetPages())
        if (data && page.id == data->gridCell.pageId)
            return page.cellHeight;
    return kMinCellHeight;
}

/**
 * @brief 计算映射文件夹图标模式下的自适应纵向间距
 * @param widget FolderMapping 组件指针
 * @return 间距像素值。根据可视高度与单元格高度的余数，
 *         在可见行之间均分以消除底部不完整行。
 *         间距过大或过小则返回 0 回退到默认紧凑布局。
 */
static int FolderMappingAdaptiveGapY(FolderMapping* widget)
{
    if (!widget) return 0;
    DesktopWidget* data = widget->GetWidgetData();
    if (!data || data->listMode) return 0;

    RECT content = FolderMappingContentRect(widget);
    int visibleHeight = content.bottom - content.top;
    if (visibleHeight <= 0) return 0;

    int cellH = FolderMappingCellHeight(widget);
    if (cellH <= 0 || visibleHeight <= cellH) return 0;

    int visibleRows = visibleHeight / cellH;
    if (visibleRows <= 1) return 0;

    int extraSpace = visibleHeight - visibleRows * cellH;
    int gapY = extraSpace / (visibleRows - 1);
    if (gapY < 0) return 0;

    int maxGap = std::max(1, static_cast<int>(cellH * kGapPercentY));
    if (gapY > maxGap) return 0;

    return gapY;
}

void FolderMapping::EnsureDateLayout() const
{
    if (!data_ || !data_->dateHeaders || IsSearchActive())
    {
        dateLayoutCache_.clear();
        dateLayoutSource_.clear();
        return;
    }

    const auto& visibleEntries = GetVisibleEntryIndices();
    if (dateLayoutSource_ == visibleEntries &&
        dateLayoutListMode_ == data_->listMode &&
        !dateLayoutCache_.empty())
        return;

    dateLayoutCache_.clear();
    dateLayoutSource_ = visibleEntries;
    dateLayoutListMode_ = data_->listMode;
    if (visibleEntries.empty()) return;

    const int headerHeight = data_->listMode ? Cu(28.0f) : Cu(36.0f);
    const int itemHeight = data_->listMode
        ? Cu(38.0f)
        : FolderMappingCellHeight(const_cast<FolderMapping*>(this));
    const int columns = data_->listMode
        ? 1
        : std::max(1, data_->gridSpan.columns);

    LONG y = 0;
    size_t groupStart = 0;
    std::wstring previousGroup;
    for (size_t i = 0; i <= visibleEntries.size(); ++i)
    {
        std::wstring currentGroup;
        if (i < visibleEntries.size() &&
            visibleEntries[i] < data_->folderEntries.size())
            currentGroup =
                FolderEntryDateGroupId(data_->folderEntries[visibleEntries[i]]);

        if (i == visibleEntries.size() ||
            (i > groupStart && currentGroup != previousGroup))
        {
            size_t groupCount = i - groupStart;
            if (groupCount > 0)
            {
                DateLayoutSegment header;
                header.isHeader = true;
                header.label = FolderEntryDateGroupLabel(previousGroup);
                header.y = y;
                header.height = headerHeight;
                dateLayoutCache_.push_back(header);
                y += headerHeight;

                DateLayoutSegment items;
                items.firstItemIndex = groupStart;
                items.itemCount = groupCount;
                items.y = y;
                items.height = data_->listMode
                    ? static_cast<LONG>(groupCount) * itemHeight
                    : static_cast<LONG>(
                        (groupCount + static_cast<size_t>(columns) - 1) /
                        static_cast<size_t>(columns)) * itemHeight;
                dateLayoutCache_.push_back(items);
                y += items.height;
            }
            groupStart = i;
        }
        if (i < visibleEntries.size())
            previousGroup = currentGroup;
    }
}

/**
 * @brief 计算映射文件夹所有条目占用的总内容高度
 * @param widget    FolderMapping 组件指针
 * @param itemCount 条目数量
 * @return 总内容高度（像素）。列表模式下每行 38 像素；
 *         图标模式下按列数计算行数乘以单元格高度并计入自适应间距
 */
static int FolderMappingContentHeight(FolderMapping* widget, size_t itemCount)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    if (data->dateHeaders && !widget->IsSearchActive())
    {
        widget->EnsureDateLayout();
        const auto& segments = widget->GetDateLayoutCache();
        return segments.empty()
            ? 0
            : static_cast<int>(segments.back().y + segments.back().height);
    }
    if (data->listMode)
        return static_cast<int>(itemCount) * widget->Cu(38.0f);
    int columns = std::max(1, data->gridSpan.columns);
    int rows = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    if (rows <= 0) return 0;
    int cellH = FolderMappingCellHeight(widget);
    int gapY = FolderMappingAdaptiveGapY(widget);
    return rows * cellH + (rows - 1) * gapY;
}

/**
 * @brief 计算映射文件夹在垂直方向上的最大滚动偏移量
 * @param widget FolderMapping 组件指针
 * @return 最大滚动偏移量（像素），确保内容底部能够滚动到可视区域底部
 */
static int FolderMappingMaxScrollOffset(FolderMapping* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    RECT content = FolderMappingContentRect(widget);
    int contentHeight = std::max<int>(1, content.bottom - content.top);
    return std::max(0, FolderMappingContentHeight(
        widget, widget->GetVisibleEntryIndices().size()) -
        contentHeight + widget->Cu(kMinCellHeight / 2.0f));
}

/**
 * @brief 根据线性索引计算文件夹条目的绘制矩形区域
 * @param widget      FolderMapping 组件指针
 * @param linearIndex 条目在线性列表中的序号
 * @return 条目所占的 RECT。列表模式下为单行水平条；
 *         图标模式下按网格列数计算行列位置
 */
static RECT FolderMappingItemRect(FolderMapping* widget, size_t linearIndex)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return {};
    RECT content = FolderMappingContentRect(widget);
    int scroll = std::clamp(data->scrollOffset, 0, FolderMappingMaxScrollOffset(widget));
    if (data->dateHeaders && !widget->IsSearchActive())
    {
        widget->EnsureDateLayout();
        const auto& segments = widget->GetDateLayoutCache();
        const FolderMapping::DateLayoutSegment* itemSegment = nullptr;
        for (const auto& segment : segments)
        {
            if (segment.isHeader) continue;
            if (linearIndex >= segment.firstItemIndex &&
                linearIndex < segment.firstItemIndex + segment.itemCount)
            {
                itemSegment = &segment;
                break;
            }
        }
        if (!itemSegment && linearIndex == widget->GetVisibleEntryIndices().size())
        {
            for (auto it = segments.rbegin(); it != segments.rend(); ++it)
            {
                if (!it->isHeader)
                {
                    itemSegment = &*it;
                    break;
                }
            }
        }
        if (itemSegment)
        {
            size_t localIndex = linearIndex - itemSegment->firstItemIndex;
            if (data->listMode)
            {
                const int itemHeight = widget->Cu(38.0f);
                RECT rect = MakeRect(content.left,
                    content.top + itemSegment->y +
                        static_cast<LONG>(localIndex * itemHeight) - scroll,
                    content.right,
                    content.top + itemSegment->y +
                        static_cast<LONG>((localIndex + 1) * itemHeight) - scroll);
                InflateRect(&rect, -widget->Cu(4.0f), -widget->Cu(2.0f));
                return rect;
            }

            int columns = std::max(1, data->gridSpan.columns);
            int col = static_cast<int>(
                localIndex % static_cast<size_t>(columns));
            int row = static_cast<int>(
                localIndex / static_cast<size_t>(columns));
            int itemWidth = std::max<int>(
                1, (content.right - content.left) / columns);
            int cellHeight = FolderMappingCellHeight(widget);
            return MakeRect(
                content.left + col * itemWidth,
                content.top + itemSegment->y + row * cellHeight - scroll,
                col + 1 == columns
                    ? content.right
                    : content.left + (col + 1) * itemWidth,
                content.top + itemSegment->y +
                    (row + 1) * cellHeight - scroll);
        }
    }
    if (data->listMode)
    {
        const int itemHeight = widget->Cu(38.0f);
        RECT rect = MakeRect(content.left,
            content.top + static_cast<LONG>(linearIndex * itemHeight) - scroll,
            content.right,
            content.top + static_cast<LONG>((linearIndex + 1) * itemHeight) - scroll);
        InflateRect(&rect, -widget->Cu(4.0f), -widget->Cu(2.0f));
        return rect;
    }

    int columns = std::max(1, data->gridSpan.columns);
    int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    int itemW = std::max<int>(1, (content.right - content.left) / columns);
    int cellH = FolderMappingCellHeight(widget);
    int gapY = FolderMappingAdaptiveGapY(widget);
    int rowStep = cellH + gapY;
    return MakeRect(
        content.left + col * itemW,
        content.top + row * rowStep - scroll,
        col + 1 == columns ? content.right : content.left + (col + 1) * itemW,
        content.top + row * rowStep + cellH - scroll);
}

/**
 * @brief 获取指定索引位置的槽位条目对象
 * @param idx 条目索引
 * @return 指向 Item 的指针，若索引无效则返回 nullptr
 *
 * 创建 FolderEntryIcon 作为条目图标并缓存到 slotItemCache_ 中，
 * 确保在槽位重建期间对象生命周期有效。
 */
Item* FolderMapping::GetSlotItem(size_t idx) const
{
    if (!data_ || idx >= data_->folderEntries.size()) return nullptr;
    auto icon = std::make_unique<FolderEntryIcon>(&data_->folderEntries[idx],
        const_cast<FolderMapping*>(this), app_);
    Item* result = icon.get();
    slotItemCache_.push_back(std::move(icon));
    return result;
}

/**
 * @brief 构建当前可见的槽位列表
 * @return 槽位独占指针的向量
 *
 * 遍历所有文件夹条目（及可选的末尾空槽位），计算每个条目
 * 在内容区域中的矩形位置，跳过完全不可见的条目以优化性能。
 */
std::vector<std::unique_ptr<Slot>> FolderMapping::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;

    const auto& visibleEntries = GetVisibleEntryIndices();
    size_t total = IncludeTrailingEmptySlot()
        ? visibleEntries.size() + 1
        : visibleEntries.size();
    for (size_t idx = 0; idx < total; ++idx)
    {
        RECT cell = FolderMappingItemRect(this, idx);
        if (IsRectEmptyRect(cell)) continue;
        auto slot = std::make_unique<Slot>(this, cell, idx);
        Item* item = idx < visibleEntries.size()
            ? GetSlotItem(visibleEntries[idx])
            : nullptr;
        if (item) item->SetBounds(cell);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }
    return slots;
}

/**
 * @brief 获取指定索引位置的拖拽源条目对象
 * @param idx 条目索引
 * @return 指向 Item 的指针，索引无效时返回 nullptr
 *
 * 与 GetSlotItem 类似，但条目缓存于 dragSourceCache_ 中，
 * 用于拖放操作期间保持条目对象的有效性。
 */
Item* FolderMapping::GetMemberItem(size_t idx) const
{
    if (!data_ || idx >= data_->folderEntries.size()) return nullptr;
    auto icon = std::make_unique<FolderEntryIcon>(&data_->folderEntries[idx],
        const_cast<FolderMapping*>(this), app_);
    Item* result = icon.get();
    dragSourceCache_.push_back(std::move(icon));
    return result;
}

/**
 * @brief 获取当前选中的条目索引列表
 * @return 选中条目的索引向量
 */
std::vector<size_t> FolderMapping::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_) return result;
    for (size_t i = 0; i < data_->folderEntries.size(); ++i)
        if (data_->folderEntries[i].selected) result.push_back(i);
    return result;
}

/**
 * @brief 重新排序文件夹条目
 * @param indices      需要移动的条目索引列表（逆序处理以保持索引正确）
 * @param insertBefore 目标插入位置（移动完成后会调整以补偿移除的索引）
 *
 * 将指定索引的条目从原位置移除，再按顺序插入到 insertBefore 指定的位置，
 * 并同步更新 data_->itemKeys 使其与新的条目顺序保持一致。
 */
void FolderMapping::ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
{
    if (!data_) return;
    if (data_->dateHeaders) return;
    std::vector<FolderEntry> moving;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        if (*it >= data_->folderEntries.size()) continue;
        moving.push_back(std::move(data_->folderEntries[*it]));
        data_->folderEntries.erase(data_->folderEntries.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    size_t adjusted = insertBefore;
    for (auto idx : indices)
        if (idx < insertBefore) --adjusted;
    if (adjusted > data_->folderEntries.size()) adjusted = data_->folderEntries.size();
    for (auto it = moving.rbegin(); it != moving.rend(); ++it)
        data_->folderEntries.insert(data_->folderEntries.begin() + static_cast<std::ptrdiff_t>(adjusted++), std::move(*it));
    data_->itemKeys.clear();
    data_->itemKeys.reserve(data_->folderEntries.size());
    for (const auto& entry : data_->folderEntries)
        data_->itemKeys.push_back(entry.fullPath);
    InvalidateSlots();
}

size_t FolderMapping::GetDropInsertIndex(
    Slot* targetSlot, HitRegion region) const
{
    const auto& visibleEntries = GetVisibleEntryIndices();
    size_t visibleInsert = targetSlot ? targetSlot->GetIndex() : visibleEntries.size();
    if (targetSlot && region == HitRegion::SortAfter)
        ++visibleInsert;
    if (visibleInsert < visibleEntries.size())
        return visibleEntries[visibleInsert];
    return data_ ? data_->folderEntries.size() : 0;
}

/**
 * @brief 获取文件夹条目总数
 * @return 条目数量
 */
size_t FolderMapping::GetSlotCount() const
{
    return data_ ? GetVisibleEntryIndices().size() : 0;
}

/**
 * @brief 获取每个条目的高度
 * @return 条目高度（像素）。列表模式下固定为 38 像素，
 *         图标模式下取当前单元格高度
 */
int FolderMapping::GetItemHeight() const
{
    return (data_ && data_->listMode)
        ? Cu(38.0f)
        : FolderMappingCellHeight(const_cast<FolderMapping*>(this));
}

/**
 * @brief 获取每个条目的宽度
 * @return 条目宽度（像素）。列表模式下返回父类默认宽度；
 *         图标模式下按内容区域宽度除以列数计算
 */
int FolderMapping::GetItemWidth() const
{
    if (!data_ || data_->listMode) return WidgetContainer::GetItemWidth();
    RECT content = FolderMappingContentRect(const_cast<FolderMapping*>(this));
    return std::max<int>(1, (content.right - content.left) / std::max(1, data_->gridSpan.columns));
}

/**
 * @brief 获取最大滚动偏移量
 * @return 最大滚动偏移值（像素），委托给 FolderMappingMaxScrollOffset
 */
int FolderMapping::GetMaxScrollOffset() const
{
    return FolderMappingMaxScrollOffset(const_cast<FolderMapping*>(this));
}

/**
 * @brief 获取所有条目的总内容高度
 * @return 总高度（像素），委托给 FolderMappingContentHeight
 */
int FolderMapping::GetTotalContentHeight() const
{
    return FolderMappingContentHeight(const_cast<FolderMapping*>(this),
        data_ ? GetVisibleEntryIndices().size() : 0);
}

/**
 * @brief 获取内容区域的可视高度
 * @return 可视区域高度（像素），即内容裁剪矩形的高度
 */
int FolderMapping::GetVisibleContentHeight() const
{
    RECT content = FolderMappingContentRect(const_cast<FolderMapping*>(this));
    return std::max(1, (int)(content.bottom - content.top));
}

/**
 * @brief 处理外部条目拖放到本组件时的回调
 * @param sourceItems 拖拽源条目列表
 * @param origin      源容器
 * @param targetSlot  目标槽位
 * @param region      命中区域
 * @param mods        键盘修饰键状态
 *
 * 通过 App 的拖放管道（BuildDragSourceList -> BuildDropPreviewList ->
 * ExecuteDropPipeline）执行完整的拖放处理流程。
 */
void FolderMapping::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

/**
 * @brief 绘制映射文件夹的内容区域
 * @param context D2D 设备上下文
 * @param body    组件主体矩形（未使用，内容区域由 FolderMappingContentRect 决定）
 *
 * 当文件夹为空时显示居中提示文字"空文件夹"。
 * 非空时根据当前模式（图标/列表）分别绘制条目，
 * 图标模式下绘制 FolderEntryIcon，列表模式下绘制列表项。
 * 绘制前会设置裁剪区域以确保内容不溢出。
 */
void FolderMapping::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    (void)body;
    bool privacyActive = data_->privacyMode && !app_->dragSession_.IsActive() && !app_->externalDragActive_ && !PtInRect(&data_->bounds, app_->lastMousePoint_);
    const bool lt = app_->IsLightContentTheme();

    DrawSearchBox(context);

    if (data_->folderEntries.empty())
    {
        RECT empty = GetBodyRect();
        InflateRect(&empty, -Cu(12.0f), -Cu(12.0f));
        IDWriteTextFormat* centered = GetCuTextFormat(13.0f, false, true);
        IDWriteTextFormat* lightCentered = lt ? GetCuTextFormatWeight(13.0f, DWRITE_FONT_WEIGHT_LIGHT, true) : nullptr;
        app_->DrawD2DText(context, _LW("widget.folder_mapping.empty"), empty,
            (lt && lightCentered) ? lightCentered :
                (centered ? centered : app_->listItemTextFormat_.Get()),
            lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f),
            DWRITE_WORD_WRAPPING_WRAP);
        return;
    }

    EnsureCategorySnapshot();
    if (data_->showFileCategories)
    {
        const std::wstring activeCategory = CachedActiveCategoryId();
        RECT tabsRect = FolderMappingTabsRect(this);
        if (!IsRectEmptyRect(tabsRect))
        {
            context->PushAxisAlignedClip(
                app_->ToD2DRect(tabsRect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (size_t i = 0; i < visibleCategoryIds_.size(); ++i)
            {
                RECT tab = FolderMappingTabRect(this, i);
                if (IsRectEmptyRect(tab)) continue;
                bool active = visibleCategoryIds_[i] == activeCategory;
                bool hovered = PtInRect(&tab, app_->lastMousePoint_) != FALSE;
                DrawCategorizedTab(
                    context, tab,
                    FolderMappingTabDisplayText(this, visibleCategoryIds_[i]),
                    active, hovered);
            }
            context->PopAxisAlignedClip();
        }
    }

    const auto& visibleEntries = GetVisibleEntryIndices();
    if (visibleEntries.empty())
    {
        RECT empty = FolderMappingContentRect(this);
        IDWriteTextFormat* centered = GetCuTextFormat(13.0f, false, true);
        IDWriteTextFormat* lightCentered = lt
            ? GetCuTextFormatWeight(13.0f, DWRITE_FONT_WEIGHT_LIGHT, true)
            : nullptr;
        app_->DrawD2DText(context, _LW("widget.categories.no_results"), empty,
            (lt && lightCentered) ? lightCentered :
                (centered ? centered : app_->listItemTextFormat_.Get()),
            lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f)
               : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f),
            DWRITE_WORD_WRAPPING_WRAP);
        return;
    }

    auto& slots = GetSlots();
    RECT content = FolderMappingContentRect(this);
    bool listMode = data_->listMode;

    context->PushAxisAlignedClip(app_->ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (data_->dateHeaders && !IsSearchActive())
    {
        EnsureDateLayout();
        IDWriteTextFormat* headerFormat =
            GetCuTextFormat(13.0f, false, false);
        IDWriteTextFormat* lightHeaderFormat = lt
            ? GetCuTextFormatWeight(
                13.0f, DWRITE_FONT_WEIGHT_LIGHT, false)
            : nullptr;
        int scroll = std::clamp(
            data_->scrollOffset, 0, FolderMappingMaxScrollOffset(this));
        for (const auto& segment : dateLayoutCache_)
        {
            if (!segment.isHeader) continue;
            RECT headerRect = MakeRect(
                content.left,
                std::max<LONG>(
                    content.top, content.top + segment.y - scroll),
                content.right,
                std::min<LONG>(
                    content.bottom,
                    content.top + segment.y + segment.height - scroll));
            if (headerRect.top >= headerRect.bottom) continue;
            RECT labelRect = headerRect;
            labelRect.left += Cu(8.0f);
            InflateRect(&labelRect, 0, -Cu(8.0f));
            app_->DrawD2DText(context, segment.label, labelRect,
                (lt && lightHeaderFormat)
                    ? lightHeaderFormat
                    : (headerFormat
                        ? headerFormat
                        : app_->listItemTextFormat_.Get()),
                lt
                    ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.52f));
        }
    }
    for (const auto& slot : slots)
    {
        if (!slot) continue;
        size_t visibleIndex = slot->GetIndex();
        if (visibleIndex >= visibleEntries.size()) continue;
        size_t entryIndex = visibleEntries[visibleIndex];
        if (entryIndex >= data_->folderEntries.size()) continue;
        const FolderEntry& entry = data_->folderEntries[entryIndex];
        RECT cell = slot->GetBounds();
        if (cell.bottom <= content.top || cell.top >= content.bottom) continue;

        if (!listMode)
        {
            if (privacyActive)
                DrawPrivacyPlaceholder(context, cell, entry.name, entry.isDirectory);
            else
            {
                RECT bodyRect = GetBodyRect();
                bool hovered = !entry.selected && PtInRect(&cell, app_->lastMousePoint_) && PtInRect(&bodyRect, app_->lastMousePoint_);
                FolderEntryIcon icon(const_cast<FolderEntry*>(&entry), this, app_);
                icon.Draw(context, cell, entry.selected ? 2 : (hovered ? 1 : 0),
                    app_->IsLightContentTheme());
            }
            continue;
        }

        if (privacyActive)
            DrawPrivacyPlaceholder(context, cell, entry.name, entry.isDirectory);
        else
            DrawListItem(context, cell, entry.iconBitmap, entry.sysIconIndex,
                entry.name, entry.selected);
    }
    context->PopAxisAlignedClip();
}

/**
 * @brief 绘制标题栏上的切换视图和打开文件夹按钮
 * @param context    D2D 设备上下文
 * @param handleRect 标题栏矩形
 * @param hovered    标题栏是否处于悬停状态（当前未使用）
 *
 * 在标题栏右侧绘制三个按钮：
 * - 日期按钮：开启或关闭按修改日期分组
 * - 切换按钮：在图标模式（网格）和列表模式之间切换
 * - 打开文件夹按钮：打开当前映射的磁盘文件夹
 * 按钮使用 Font Awesome 图标，并具有悬停高亮效果。
 */
void FolderMapping::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    if (!data_ || !app_) return;
    const bool lt = app_->IsLightContentTheme();

    const float bs = GetBarScale();
    const int btnSize = Cu(14.0f * bs);
    const int gap = Cu(4.0f * bs);
    const int gapBetween = Cu(7.0f * bs);
    const int resizeReserve = Cu(20.0f * bs);
    const int h = handleRect.bottom - handleRect.top;
    RECT toggleBtn = {
        handleRect.right - resizeReserve - gap - btnSize - gapBetween - btnSize,
        handleRect.top + (h - btnSize) / 2,
        handleRect.right - resizeReserve - gap - btnSize - gapBetween,
        handleRect.top + (h + btnSize) / 2
    };
    RECT dateBtn = {
        toggleBtn.left - gapBetween - btnSize,
        handleRect.top + (h - btnSize) / 2,
        toggleBtn.left - gapBetween,
        handleRect.top + (h + btnSize) / 2
    };
    RECT openBtn = {
        handleRect.right - resizeReserve - gap - btnSize,
        handleRect.top + (h - btnSize) / 2,
        handleRect.right - resizeReserve - gap,
        handleRect.top + (h + btnSize) / 2
    };

    IDWriteTextFormat* faFormat = GetCuFaTextFormat(14.0f * bs);

    auto drawFaButton = [&](RECT rect, const std::wstring& glyph, bool active) {
        bool hot = PtInRect(&rect, app_->lastMousePoint_) != FALSE;
        app_->DrawD2DText(context, glyph, rect,
            faFormat ? faFormat :
                (app_->faTextFormat_ ? app_->faTextFormat_.Get() : app_->listItemTextFormat_.Get()),
            lt
                ? (active
                    ? (hot ? D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.85f)
                           : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.50f))
                    : (hot ? D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.45f)
                           : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.25f)))
                : (active
                    ? (hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f)
                           : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.60f))
                    : (hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f)
                           : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f))));
    };

    drawFaButton(dateBtn, L"", data_->dateHeaders);
    drawFaButton(toggleBtn, data_->listMode ? L"" : L"", true);
    drawFaButton(openBtn, L"", true);
    (void)hovered;
}

/**
 * @brief 测试鼠标点击位置命中了哪个组件区域
 * @param pt 鼠标点击坐标
 * @return 命中类型。若命中了标题栏，进一步判断是否命中
 *         视图切换按钮或打开文件夹按钮
 */
std::wstring FolderMapping::CategoryIdAtPoint(POINT pt) const
{
    if (!data_ || !data_->showFileCategories) return L"";
    EnsureCategorySnapshot();
    for (size_t i = 0; i < visibleCategoryIds_.size(); ++i)
    {
        RECT tab = FolderMappingTabRect(const_cast<FolderMapping*>(this), i);
        if (PtInRect(&tab, pt))
            return visibleCategoryIds_[i];
    }
    return L"";
}

bool FolderMapping::TryScrollTabs(POINT pt, int delta)
{
    if (!data_ || !app_ || !data_->showFileCategories) return false;
    RECT tabs = FolderMappingTabsRect(this);
    if (IsRectEmptyRect(tabs) || !PtInRect(&tabs, pt)) return false;
    std::vector<int> widths =
        FolderMappingTabWidths(this, tabs.right - tabs.left);
    int maxScroll = std::max(0,
        FolderMappingTabTotalWidth(widths) -
        static_cast<int>(tabs.right - tabs.left));
    if (maxScroll <= 0) return false;
    data_->tabScrollOffset =
        std::clamp(data_->tabScrollOffset - delta / 2, 0, maxScroll);
    return true;
}

WidgetHit FolderMapping::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base == WidgetHit::None || !data_) return base;

    if (!CategoryIdAtPoint(pt).empty())
        return WidgetHit::CategoryTab;
    RECT searchRect = GetSearchBoxRect();
    if (!IsRectEmptyRect(searchRect) && PtInRect(&searchRect, pt))
        return WidgetHit::SearchBox;
    if (base != WidgetHit::MoveHandle) return base;

    RECT handle = GetMoveHandleRect();
    const float bs = GetBarScale();
    const int btnSize = Cu(14.0f * bs);
    const int gap = Cu(4.0f * bs);
    const int gapBetween = Cu(7.0f * bs);
    const int resizeReserve = Cu(20.0f * bs);
    const int h = handle.bottom - handle.top;
    RECT toggleBtn = {
        handle.right - resizeReserve - gap - btnSize - gapBetween - btnSize,
        handle.top + (h - btnSize) / 2,
        handle.right - resizeReserve - gap - btnSize - gapBetween,
        handle.top + (h + btnSize) / 2
    };
    RECT dateBtn = {
        toggleBtn.left - gapBetween - btnSize,
        handle.top + (h - btnSize) / 2,
        toggleBtn.left - gapBetween,
        handle.top + (h + btnSize) / 2
    };
    RECT openBtn = {
        handle.right - resizeReserve - gap - btnSize,
        handle.top + (h - btnSize) / 2,
        handle.right - resizeReserve - gap,
        handle.top + (h + btnSize) / 2
    };
    if (PtInRect(&dateBtn, pt)) return WidgetHit::DateHeaderToggleBtn;
    if (PtInRect(&toggleBtn, pt)) return WidgetHit::ListToggleBtn;
    if (PtInRect(&openBtn, pt)) return WidgetHit::OpenFolderBtn;
    return base;
}

/**
 * @brief 获取当前选中的所有条目对象
 * @return 选中条目的 Item 指针向量
 *
 * 遍历当前槽位，收集所有被选中（entry.selected == true）的条目，
 * 创建对应的 FolderEntryIcon 并缓存到 dragSourceCache_ 中
 * 以支持拖放操作。
 */
std::vector<Item*> FolderMapping::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_) return result;

    for (const auto& slot : const_cast<FolderMapping*>(this)->GetSlots())
    {
        if (!slot) continue;
        Item* slotItem = slot->GetItem();
        auto* slotIcon = dynamic_cast<FolderEntryIcon*>(slotItem);
        FolderEntry* entry = slotIcon ? slotIcon->GetFolderEntry() : nullptr;
        if (!entry || !entry->selected) continue;

        auto icon = std::make_unique<FolderEntryIcon>(entry, const_cast<FolderMapping*>(this), app_);
        icon->SetBounds(slot->GetBounds());
        result.push_back(icon.get());
        dragSourceCache_.push_back(std::move(icon));
    }
    return result;
}
