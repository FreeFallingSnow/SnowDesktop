#pragma once

#include "constants.h"
#include "font_cu_rules.h"
#include "icon_render_rules.h"
#include "item_layout_rules.h"

#include <algorithm>
#include <cmath>

namespace snowdesktop
{

struct PageItemVisualMetrics
{
    float layoutScale = 1.0f;
    float fontScale = 1.0f;
    float fontSize = static_cast<float>(kItemFontSize);
    int iconSize = 1;
    int listIconSize = 1;
    int titleGap = 1;
    int titleHeight = 1;
    int topInset = 1;
    int sideInset = 1;
    int minimumGridWidth = 1;
    int minimumGridHeight = 1;
    int minimumListHeight = 1;
};

inline RECT ResolveGridItemIconRect(
    RECT bounds, const PageItemVisualMetrics& metrics)
{
    const int cellWidth = std::max<LONG>(1, bounds.right - bounds.left);
    const int cellHeight = std::max<LONG>(1, bounds.bottom - bounds.top);
    const int iconX = bounds.left + (cellWidth - metrics.iconSize) / 2;
    // Local widget tracks can be taller than the page's minimum visual block.
    // Split that spare height above and below the complete icon/title region
    // so the first row does not look pinned to the glass frame.
    const int visualOffset = std::max(
        0, (cellHeight - metrics.minimumGridHeight) / 2);
    const int iconY = bounds.top + visualOffset + metrics.topInset;
    return RECT{ iconX, iconY,
        iconX + metrics.iconSize, iconY + metrics.iconSize };
}

inline RECT ResolveListItemIconRect(
    RECT row, int left, const PageItemVisualMetrics& metrics)
{
    const int rowHeight = std::max<LONG>(1, row.bottom - row.top);
    const int availableWidth = std::max<LONG>(1, row.right - left);
    const int iconSize = std::clamp(
        metrics.listIconSize, 1, std::min(rowHeight, availableWidth));
    const int iconY = row.top + (rowHeight - iconSize) / 2;
    return RECT{ left, iconY,
        left + iconSize, iconY + iconSize };
}

inline RECT ResolveIconOnlyHighlightRect(
    RECT bounds, RECT iconRect, float layoutScale)
{
    const int padding = std::max(1, static_cast<int>(std::round(
        4.0f * std::max(0.1f, layoutScale))));
    RECT result = iconRect;
    InflateRect(&result, padding, padding);
    result.left = std::max(result.left, bounds.left);
    result.top = std::max(result.top, bounds.top);
    result.right = std::min(result.right, bounds.right);
    result.bottom = std::min(result.bottom, bounds.bottom);
    return result;
}

inline RECT ResolveGridItemTitleRect(
    RECT bounds, int top, int height)
{
    // Grid tracks already reserve the requested horizontal gap. Using the
    // complete local cell width keeps that visible clearance aligned with the
    // vertical clearance below the complete icon-and-title region instead of
    // adding another title-only inset on both sides.
    return RECT{ bounds.left, top, bounds.right,
        top + std::max(1, height) };
}

inline PageItemVisualMetrics ResolvePageItemVisualMetrics(
    int pitchWidth, int pitchHeight, float itemFontSizeCu,
    float iconSizeScale = kDefaultItemIconSizeScale)
{
    PageItemVisualMetrics result;
    result.layoutScale = std::max(0.1f, std::min(
        static_cast<float>(std::max(1, pitchWidth)) /
            static_cast<float>(kCellWidth),
        static_cast<float>(std::max(1, pitchHeight)) /
            static_cast<float>(kMinCellHeight)));
    result.fontScale = result.layoutScale;
    result.fontSize = font_cu_rules::Scale(
        itemFontSizeCu, result.fontScale);
    const float lineHeight = result.fontSize * 7.0f / 6.0f;
    result.titleHeight = item_layout_rules::CollapsedTextHeight(lineHeight);
    result.titleGap = item_layout_rules::TitleGap(result.layoutScale);
    result.topInset = std::max(1, static_cast<int>(std::round(
        2.0f * result.layoutScale)));
    result.sideInset = result.topInset;

    // Resolve the icon against the cell that the page would have at the
    // default spacing. The visual size therefore follows the page pitch and
    // does not change while the user adjusts spacing.
    const int referenceGapX = std::max(0, static_cast<int>(std::round(
        std::max(1, pitchWidth) * kGapPercentX)));
    const int referenceGapY = std::max(0, static_cast<int>(std::round(
        std::max(1, pitchHeight) * kGapPercentY)));
    const int referenceWidth = std::max(1, pitchWidth - referenceGapX);
    const int referenceHeight = std::max(1, pitchHeight - referenceGapY);
    const int availableWidth = std::max(
        1, referenceWidth - result.sideInset * 2);
    const int availableHeight = item_layout_rules::AvailableIconHeight(
        referenceHeight, result.topInset,
        result.titleGap, result.titleHeight);
    const int defaultIconSize = std::clamp(
        std::min(availableWidth, availableHeight), 1,
        icon_render_rules::kMaximumSourcePixels);
    const int maximumIconWidth = std::max(
        1, std::max(1, pitchWidth) - result.sideInset * 2);
    const int maximumIconHeight = item_layout_rules::AvailableIconHeight(
        std::max(1, pitchHeight), result.topInset,
        result.titleGap, result.titleHeight);
    const int maximumIconSize = std::clamp(
        std::min(maximumIconWidth, maximumIconHeight), 1,
        icon_render_rules::kMaximumSourcePixels);
    result.iconSize = std::clamp(
        static_cast<int>(std::round(defaultIconSize * std::clamp(
            iconSizeScale, kMinimumItemIconSizeScale,
            kMaximumItemIconSizeScale))),
        1, maximumIconSize);
    // List rows use a deliberately compact visual baseline. They still follow
    // the page-level icon-size setting, but must not inherit the much larger
    // desktop-grid icon edge length.
    result.listIconSize = std::clamp(
        static_cast<int>(std::round(
            kListItemIconSize * result.layoutScale * std::clamp(
                iconSizeScale, kMinimumItemIconSizeScale,
                kMaximumItemIconSizeScale))),
        1, icon_render_rules::kMaximumSourcePixels);
    result.minimumGridWidth = result.iconSize + result.sideInset * 2;
    result.minimumGridHeight = result.topInset + result.iconSize +
        result.titleGap + result.titleHeight;
    result.minimumListHeight = result.listIconSize +
        std::max(2, static_cast<int>(std::round(
            4.0f * result.layoutScale)));
    return result;
}

} // namespace snowdesktop
