#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/utils.h"
#include "layout/item_layout_rules.h"
#include "icons/icon_render_rules.h"
#include "app/navigation/quick_navigation_helpers.h"

// Render geometry and item layout metrics.

D2D1_RECT_F DesktopApp::ToD2DRect(const RECT& r)
{
    return snowdesktop::ToD2DRect(r);
}

void DesktopApp::DrawD2DSeparator(ID2D1RenderTarget* ctx, RECT rect, const D2D1_COLOR_F& color)
{
    drawing_.DrawSeparator(ctx, rect, color);
}

snowdesktop::PageItemVisualMetrics DesktopApp::GetPageItemVisualMetrics(
    const GridPage& page) const
{
    return snowdesktop::ResolvePageItemVisualMetrics(
        page.itemPitchWidth, page.itemPitchHeight,
        itemFontSizeCu_, itemIconSizeScale_);
}

snowdesktop::PageItemVisualMetrics DesktopApp::GetItemVisualMetrics(
    RECT bounds, int componentTitleLines) const
{
    const POINT center = {
        bounds.left + (bounds.right - bounds.left) / 2,
        bounds.top + (bounds.bottom - bounds.top) / 2
    };
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, center))
            return snowdesktop::ResolveWidgetTitleMetrics(
                GetPageItemVisualMetrics(page), componentTitleLines);
    }
    return snowdesktop::ResolveWidgetTitleMetrics(snowdesktop::ResolvePageItemVisualMetrics(
        kCellWidth, kMinCellHeight, itemFontSizeCu_,
        itemIconSizeScale_), componentTitleLines);
}

float DesktopApp::GetItemLayoutScale(RECT bounds) const
{
    return GetItemVisualMetrics(bounds).layoutScale;
}

RECT DesktopApp::GetItemIconRect(RECT bounds, int componentTitleLines) const
{
    const auto metrics = GetItemVisualMetrics(bounds, componentTitleLines);
    const int cellW = bounds.right - bounds.left;
    const int cellH = bounds.bottom - bounds.top;
    if (cellH < static_cast<int>(std::round(
            50.0f * metrics.layoutScale)))
    {
        const int iconSz = std::clamp(std::min({
            metrics.iconSize,
            std::max(1, cellW - metrics.sideInset * 2),
            std::max(1, cellH - metrics.topInset * 2) }), 1,
            snowdesktop::icon_render_rules::kMaximumSourcePixels);
        return MakeRect(
            bounds.left + (cellW - iconSz) / 2,
            bounds.top + (cellH - iconSz) / 2,
            bounds.left + (cellW + iconSz) / 2,
            bounds.top + (cellH + iconSz) / 2);
    }
    // The page owns the visual size. Local cells may have different widths,
    // but spacing is clamped before layout so the complete visual remains.
    return snowdesktop::ResolveGridItemIconRect(bounds, metrics);
}

RECT DesktopApp::GetQuickNavItemIconRect(RECT bounds) const
{
    const int cellW = std::max<LONG>(1, bounds.right - bounds.left);
    const int cellH = std::max<LONG>(1, bounds.bottom - bounds.top);
    const int inset = std::max(1, QuickNavScale(2));
    const float fontSize = quickNavigation_->itemTextFormat ? quickNavigation_->itemTextFormat->GetFontSize()
        : static_cast<float>(QuickNavScale(navigationSettings_.layout.fontSize));
    const int titleBandH = QuickNavigationItemTextHeight(fontSize, navigationSettings_.layout.labelLines);
    const int titleGap = std::max(1, QuickNavScale(10));
    const int maxIconW = std::max(1, cellW - inset * 2);
    const int maxIconH = std::max(1, cellH - titleBandH - titleGap - inset * 2);
    const int iconSz = std::max(1, std::min({
        QuickNavScale(navigationSettings_.layout.iconSize),
        maxIconW,
        maxIconH
    }));
    const int iconX = bounds.left + (cellW - iconSz) / 2;
    // Center the complete icon/title group in the shared hover and hit cell.
    const int visualHeight = iconSz + titleGap + titleBandH;
    const int iconY = bounds.top + std::max(inset, (cellH - visualHeight) / 2);
    return MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
}

int DesktopApp::ResolveItemTitleLines(const DesktopWidget* widget, bool popupHosted) const
{
    return snowdesktop::item_layout_rules::ResolveTitleLines(widget != nullptr,
        widget && widget->type == DesktopWidgetType::Collection && !widget->scrollContainerMode,
        popupHosted, desktopTitleLines_, largeFolderTitleLines_, scrollingTitleLines_);
}

RECT DesktopApp::GetItemTextRect(RECT bounds, bool expanded, int componentTitleLines) const
{
    const auto metrics = GetItemVisualMetrics(bounds, componentTitleLines);
    RECT iconRect = GetItemIconRect(bounds, componentTitleLines);
    const int textTop = iconRect.bottom +
        metrics.titleGap;
    const float lineHeight = metrics.fontSize * 7.0f / 6.0f;
    const int textH = expanded
        ? std::max(
            static_cast<int>(std::round(
                kTextExpandedHeight * metrics.layoutScale)),
            static_cast<int>(std::ceil(lineHeight * 3.0f)))
        : metrics.titleHeight;

    // The collapsed label is clipped just before a third line can begin.
    // Selected labels intentionally extend below the cell to reveal the lines
    // hidden in the normal two-line state.
    return snowdesktop::ResolveGridItemTitleRect(
        bounds, textTop, textH);
}

RECT DesktopApp::GetItemSelectionRect(RECT bounds, bool expanded, int componentTitleLines) const
{
    const float layoutScale = GetItemVisualMetrics(bounds).layoutScale;
    RECT textRect = GetItemTextRect(bounds, expanded, componentTitleLines);
    RECT selection = UnionCopy(GetItemIconRect(bounds, componentTitleLines), textRect);
    const int verticalPad = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    selection.left = bounds.left;
    selection.top = std::max(bounds.top, selection.top - verticalPad);
    selection.right = bounds.right;
    selection.bottom = std::min(bounds.bottom - verticalPad, textRect.bottom);
    return selection;
}

RECT DesktopApp::GetItemRenameRect(RECT bounds, int componentTitleLines) const
{
    return GetItemTextRect(bounds, true, componentTitleLines);
}
