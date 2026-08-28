#pragma once

#include "menu_fluent_glyphs.h"
#include "name_pinyin.h"
#include "navigation_settings.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::quick_navigation_rules
{
inline QuickNavigationDesktopViewMode
NextQuickNavigationDesktopViewMode(
    QuickNavigationDesktopViewMode mode)
{
    switch (mode)
    {
    case QuickNavigationDesktopViewMode::Tile:
        return QuickNavigationDesktopViewMode::Source;
    case QuickNavigationDesktopViewMode::Source:
        return QuickNavigationDesktopViewMode::Initial;
    case QuickNavigationDesktopViewMode::Initial:
    default:
        return QuickNavigationDesktopViewMode::Tile;
    }
}

inline std::wstring_view
QuickNavigationDesktopViewModeGlyph(
    QuickNavigationDesktopViewMode mode)
{
    switch (mode)
    {
    case QuickNavigationDesktopViewMode::Source:
        return menu_fluent_glyphs::
            kQuickNavigationSourceView;
    case QuickNavigationDesktopViewMode::Initial:
        return menu_fluent_glyphs::
            kQuickNavigationInitialView;
    case QuickNavigationDesktopViewMode::Tile:
    default:
        return menu_fluent_glyphs::
            kQuickNavigationTileView;
    }
}

inline int TabStripLabelStart(
    int tabsLeft, bool modeButtonVisible,
    int buttonWidth, int gap)
{
    return tabsLeft +
        (modeButtonVisible
            ? std::max(0, buttonWidth) +
                std::max(0, gap)
            : 0);
}

inline wchar_t InitialJumpBucketAt(
    size_t index)
{
    return index < 26
        ? static_cast<wchar_t>(L'A' + index)
        : L'#';
}

inline size_t InitialJumpBucketIndex(
    wchar_t bucket)
{
    if (bucket >= L'A' && bucket <= L'Z')
        return static_cast<size_t>(
            bucket - L'A');
    return 26;
}

struct SectionLayout
{
    size_t firstItem = 0;
    size_t itemCount = 0;
    int headerTop = 0;
    int gridTop = 0;
    int bottom = 0;
};

struct SectionItemCell
{
    int column = 0;
    int row = 0;
    int top = 0;
};

inline std::vector<SectionLayout> BuildSectionLayouts(
    const std::vector<size_t>& itemCounts,
    int columns, int cellHeight, int rowGap,
    int headerHeight, int headerGap,
    int sectionGap)
{
    columns = std::max(1, columns);
    cellHeight = std::max(1, cellHeight);
    rowGap = std::max(0, rowGap);
    headerHeight = std::max(0, headerHeight);
    headerGap = std::max(0, headerGap);
    sectionGap = std::max(0, sectionGap);

    std::vector<SectionLayout> result;
    result.reserve(itemCounts.size());
    size_t firstItem = 0;
    int y = 0;
    for (size_t count : itemCounts)
    {
        if (count == 0) continue;
        const int rows =
            (static_cast<int>(count) + columns - 1) /
            columns;
        const int gridHeight =
            rows * cellHeight +
            std::max(0, rows - 1) * rowGap;
        SectionLayout layout;
        layout.firstItem = firstItem;
        layout.itemCount = count;
        layout.headerTop = y;
        layout.gridTop = y + headerHeight + headerGap;
        layout.bottom = layout.gridTop + gridHeight;
        result.push_back(layout);
        firstItem += count;
        y = layout.bottom + sectionGap;
    }
    return result;
}

inline int SectionedContentHeight(
    const std::vector<SectionLayout>& layouts,
    int sectionGap)
{
    (void)sectionGap;
    if (layouts.empty()) return 0;
    return std::max(0, layouts.back().bottom);
}

inline const SectionLayout* FindItemSection(
    const std::vector<SectionLayout>& layouts,
    size_t itemIndex)
{
    for (const auto& layout : layouts)
        if (itemIndex >= layout.firstItem &&
            itemIndex <
                layout.firstItem + layout.itemCount)
            return &layout;
    return nullptr;
}

inline bool TryGetSectionItemCell(
    const std::vector<SectionLayout>& layouts,
    size_t itemIndex, int columns,
    int cellHeight, int rowGap,
    SectionItemCell& result)
{
    const SectionLayout* section =
        FindItemSection(layouts, itemIndex);
    if (!section) return false;
    columns = std::max(1, columns);
    const size_t localIndex =
        itemIndex - section->firstItem;
    result.column = static_cast<int>(
        localIndex %
        static_cast<size_t>(columns));
    result.row = static_cast<int>(
        localIndex /
        static_cast<size_t>(columns));
    result.top = section->gridTop +
        result.row *
            (std::max(1, cellHeight) +
                std::max(0, rowGap));
    return true;
}

inline std::vector<int> AssignSourceOwners(
    const std::vector<std::wstring>& itemKeys,
    const std::vector<
        std::vector<std::wstring>>& sourceKeys)
{
    std::unordered_map<std::wstring, int>
        firstOwner;
    for (size_t sourceIndex = 0;
        sourceIndex < sourceKeys.size();
        ++sourceIndex)
    {
        for (const auto& key :
            sourceKeys[sourceIndex])
            firstOwner.try_emplace(
                key,
                static_cast<int>(sourceIndex));
    }

    std::vector<int> result(
        itemKeys.size(), -1);
    for (size_t itemIndex = 0;
        itemIndex < itemKeys.size();
        ++itemIndex)
    {
        const auto it =
            firstOwner.find(itemKeys[itemIndex]);
        if (it != firstOwner.end())
            result[itemIndex] = it->second;
    }
    return result;
}

/**
 * @brief 按顶部标签 ID 顺序排列符合条件的组件索引。
 *
 * 标签中已登记的组件优先；尚未登记但符合条件的组件按桌面数据中的稳定
 * 顺序追加。顶部标签和聚合页必须共同调用此函数，避免各自维护排序逻辑。
 */
inline std::vector<size_t> OrderIndicesByTabIds(
    const std::vector<std::wstring>& tabOrder,
    const std::vector<std::wstring>& itemIds,
    const std::vector<bool>& eligible)
{
    const size_t count =
        std::min(itemIds.size(), eligible.size());
    std::vector<size_t> result;
    result.reserve(count);
    std::vector<bool> seen(count, false);

    for (const auto& id : tabOrder)
    {
        for (size_t index = 0;
            index < count; ++index)
        {
            if (!seen[index] &&
                eligible[index] &&
                itemIds[index] == id)
            {
                result.push_back(index);
                seen[index] = true;
                break;
            }
        }
    }

    for (size_t index = 0;
        index < count; ++index)
    {
        if (eligible[index] && !seen[index])
            result.push_back(index);
    }
    return result;
}

inline wchar_t InitialBucket(
    const std::wstring& name)
{
    if (name.empty()) return L'#';
    const std::string key =
        BuildNamePinyinFullKey(
            std::wstring(1, name.front()));
    if (key.empty()) return L'#';
    const unsigned char first =
        static_cast<unsigned char>(key.front());
    if (first >= 'a' && first <= 'z')
        return static_cast<wchar_t>(
            first - 'a' + L'A');
    if (first >= 'A' && first <= 'Z')
        return static_cast<wchar_t>(first);
    return L'#';
}

inline std::string InitialSortKey(
    const std::wstring& name)
{
    std::string key =
        BuildNamePinyinFullKey(name);
    std::transform(
        key.begin(), key.end(), key.begin(),
        [](unsigned char ch) {
            return ch >= 'a' && ch <= 'z'
                ? static_cast<char>(ch - 'a' + 'A')
                : static_cast<char>(ch);
        });
    return key;
}

inline bool InitialNameLess(
    const std::wstring& lhs,
    const std::wstring& rhs)
{
    const std::string leftKey =
        InitialSortKey(lhs);
    const std::string rightKey =
        InitialSortKey(rhs);
    if (leftKey != rightKey)
        return leftKey < rightKey;

    auto normalized = [](
        std::wstring value) {
        std::transform(
            value.begin(), value.end(),
            value.begin(),
            [](wchar_t ch) {
                return static_cast<wchar_t>(
                    std::towupper(ch));
            });
        return value;
    };
    return normalized(lhs) < normalized(rhs);
}

/**
 * Build the stable identity used by the application-icon bitmap cache.
 * Shell system-image-list indexes are deliberately excluded: Explorer may
 * replace or reuse those process-external slots after an app was indexed.
 */
inline std::wstring ApplicationIconCacheIdentity(
    std::wstring_view parsingName,
    std::wstring_view displayName)
{
    const std::wstring_view source = parsingName.empty()
        ? displayName
        : parsingName;
    std::wstring identity(source);
    std::transform(
        identity.begin(), identity.end(), identity.begin(),
        [](wchar_t ch) {
            return static_cast<wchar_t>(std::towupper(ch));
        });
    return identity;
}

inline int TabStripMaxScrollOffset(
    int contentWidth, int scrollLeft,
    int clipRight)
{
    const int available =
        std::max(1, clipRight - scrollLeft);
    return std::max(
        0, contentWidth - available);
}

constexpr bool ShouldCloseOnDeactivate(
    bool activatedWithinInteractionSurface)
{
    return !activatedWithinInteractionSurface;
}

constexpr bool ShouldOpenFromDockSearchPress(
    bool dismissedBySamePress)
{
    return !dismissedBySamePress;
}

inline bool ShouldAcceptPointerHit(
    bool open,
    POINT desktopPoint,
    const RECT& panelRect,
    const RECT& animatedVisualRect)
{
    // The opening/closing visual travels between the invocation point and the
    // final panel. Only the final panel owns input; otherwise the animated
    // visual can temporarily cover the Dock/desktop and starve their hover
    // messages until the animation finishes.
    return open &&
        !IsRectEmpty(&panelRect) &&
        !IsRectEmpty(&animatedVisualRect) &&
        PtInRect(&panelRect, desktopPoint) &&
        PtInRect(&animatedVisualRect, desktopPoint);
}
}
