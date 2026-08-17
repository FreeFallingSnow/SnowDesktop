#include "app.h"

// Render geometry and item layout metrics.

D2D1_RECT_F DesktopApp::ToD2DRect(const RECT& r)
{
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
        static_cast<float>(r.right), static_cast<float>(r.bottom));
}

std::uint64_t D2DColorBrushKey(const D2D1_COLOR_F& c)
{
    const auto quantize = [](float value) -> std::uint64_t {
        return static_cast<std::uint64_t>(std::lround(
            std::clamp(value, 0.0f, 1.0f) * 65535.0f));
    };
    return quantize(c.r) |
        (quantize(c.g) << 16) |
        (quantize(c.b) << 32) |
        (quantize(c.a) << 48);
}

/** @brief 将 GDI COLORREF（0x00BBGGRR）转换为 D2D1_COLOR_F，可选 alpha。 */
D2D1_COLOR_F ToD2DColor(COLORREF c, float a)
{
    // COLORREF 是 0x00BBGGRR；D2D1::ColorF(UINT32) 期望 0xRRGGBB，故显式按通道构造。
    return D2D1::ColorF(
        GetRValue(c) / 255.0f,
        GetGValue(c) / 255.0f,
        GetBValue(c) / 255.0f,
        a);
}

/** @brief 用 D2D 绘制一条 1 像素粗的水平/垂直分隔线。 */
void DesktopApp::DrawD2DSeparator(ID2D1RenderTarget* ctx, RECT rect, const D2D1_COLOR_F& color)
{
    if (!ctx || IsRectEmptyRect(rect)) return;
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const std::uint64_t key = D2DColorBrushKey(color);
    auto it = brushCache_.find(key);
    if (it == brushCache_.end())
    {
        ComPtr<ID2D1SolidColorBrush> b;
        if (FAILED(ctx->CreateSolidColorBrush(color, &b)) || !b) return;
        it = brushCache_.emplace(key, std::move(b)).first;
    }
    if (it != brushCache_.end() && it->second)
        ctx->FillRectangle(ToD2DRect(rect), it->second.Get());
}

snowdesktop::PageItemVisualMetrics DesktopApp::GetPageItemVisualMetrics(
    const GridPage& page) const
{
    return snowdesktop::ResolvePageItemVisualMetrics(
        page.itemPitchWidth, page.itemPitchHeight,
        itemFontSizeCu_, itemIconSizeScale_);
}

snowdesktop::PageItemVisualMetrics DesktopApp::GetItemVisualMetrics(
    RECT bounds) const
{
    const POINT center = {
        bounds.left + (bounds.right - bounds.left) / 2,
        bounds.top + (bounds.bottom - bounds.top) / 2
    };
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, center))
            return GetPageItemVisualMetrics(page);
    }
    return snowdesktop::ResolvePageItemVisualMetrics(
        kCellWidth, kMinCellHeight, itemFontSizeCu_,
        itemIconSizeScale_);
}

float DesktopApp::GetItemLayoutScale(RECT bounds) const
{
    return GetItemVisualMetrics(bounds).layoutScale;
}

RECT DesktopApp::GetItemIconRect(RECT bounds) const
{
    const auto metrics = GetItemVisualMetrics(bounds);
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
    const int titleBandH = std::max(1, QuickNavScale(kQuickNavigationTextHeight));
    const int titleGap = std::max(1, QuickNavScale(2));
    const int maxIconW = std::max(1, cellW - inset * 2);
    const int maxIconH = std::max(1, cellH - titleBandH - titleGap - inset);
    const int iconSz = std::max(1, std::min({
        QuickNavScale(48),
        maxIconW,
        maxIconH
    }));
    const int iconX = bounds.left + (cellW - iconSz) / 2;
    const int iconY = bounds.top + inset;
    return MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
}

RECT DesktopApp::GetItemTextRect(RECT bounds, bool expanded) const
{
    const auto metrics = GetItemVisualMetrics(bounds);
    RECT iconRect = GetItemIconRect(bounds);
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

RECT DesktopApp::GetItemSelectionRect(RECT bounds, bool expanded) const
{
    const float layoutScale = GetItemVisualMetrics(bounds).layoutScale;
    RECT textRect = GetItemTextRect(bounds, expanded);
    RECT selection = UnionCopy(GetItemIconRect(bounds), textRect);
    const int verticalPad = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    selection.left = bounds.left;
    selection.top = std::max(bounds.top, selection.top - verticalPad);
    selection.right = bounds.right;
    selection.bottom = std::min(bounds.bottom - verticalPad, textRect.bottom);
    return selection;
}
