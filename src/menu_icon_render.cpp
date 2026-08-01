#include "menu_icon_render.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace snowdesktop::menu_icon
{
namespace
{

int Scale(int value, UINT dpi)
{
    return std::max(1, MulDiv(value, static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

void FillSolidRect(HDC dc, const RECT& bounds, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush)
        return;
    FillRect(dc, &bounds, brush);
    DeleteObject(brush);
}

void FillRoundedRect(HDC dc, const RECT& bounds, int radius,
    COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush)
        return;
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
        radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

SIZE MeasureText(HDC dc, const wchar_t* text, int length)
{
    SIZE result{};
    if (text && length > 0)
        GetTextExtentPoint32W(dc, text, length, &result);
    return result;
}

void DrawCheckmark(HDC dc, const RECT& bounds, const Metrics& metrics,
    COLORREF color)
{
    const int centerX = bounds.left + metrics.leftPadding +
        metrics.iconColumnWidth / 2;
    const int centerY = (bounds.top + bounds.bottom) / 2;
    const int stroke = std::max(1, metrics.rowHeight / 17);
    HPEN pen = CreatePen(PS_SOLID, stroke, color);
    if (!pen)
        return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, centerX - metrics.iconColumnWidth / 4, centerY, nullptr);
    LineTo(dc, centerX - 1, centerY + metrics.iconColumnWidth / 5);
    LineTo(dc, centerX + metrics.iconColumnWidth / 3,
        centerY - metrics.iconColumnWidth / 4);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawSubmenuArrow(HDC dc, const RECT& bounds,
    const Metrics& metrics, COLORREF color)
{
    const int centerX = bounds.right - metrics.rightPadding -
        metrics.arrowColumnWidth / 2;
    const int centerY = (bounds.top + bounds.bottom) / 2;
    const int halfHeight = std::max(2, metrics.arrowColumnWidth / 5);
    HPEN pen = CreatePen(PS_SOLID,
        std::max(1, metrics.rowHeight / 24), color);
    if (!pen)
        return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, centerX - halfHeight / 2, centerY - halfHeight, nullptr);
    LineTo(dc, centerX + halfHeight / 2, centerY);
    LineTo(dc, centerX - halfHeight / 2, centerY + halfHeight);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

} // namespace

Palette ResolvePalette(bool lightTheme)
{
    if (lightTheme)
    {
        return {
            RGB(250, 250, 250),
            RGB(232, 232, 232),
            RGB(26, 26, 26),
            RGB(118, 118, 118),
            RGB(225, 225, 225),
        };
    }
    return {
        RGB(44, 44, 44),
        RGB(56, 56, 56),
        RGB(255, 255, 255),
        RGB(158, 158, 158),
        RGB(68, 68, 68),
    };
}

Metrics ResolveMetrics(UINT dpi)
{
    const UINT effectiveDpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
    return {
        Scale(34, effectiveDpi),
        Scale(9, effectiveDpi),
        Scale(200, effectiveDpi),
        Scale(4, effectiveDpi),
        Scale(2, effectiveDpi),
        Scale(4, effectiveDpi),
        Scale(10, effectiveDpi),
        Scale(22, effectiveDpi),
        Scale(8, effectiveDpi),
        Scale(10, effectiveDpi),
        Scale(18, effectiveDpi),
    };
}

SIZE MeasureItem(HDC dc, HFONT textFont, const ItemView& item,
    const Metrics& metrics)
{
    if (item.separator)
        return { static_cast<LONG>(metrics.minimumWidth),
            static_cast<LONG>(metrics.separatorHeight) };

    HGDIOBJ oldFont = nullptr;
    if (dc && textFont)
        oldFont = SelectObject(dc, textFont);

    const std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    const std::wstring_view primary = label.substr(0, tab);
    const std::wstring_view shortcut = tab == std::wstring_view::npos
        ? std::wstring_view{} : label.substr(tab + 1);
    const SIZE primarySize = MeasureText(dc, primary.data(),
        static_cast<int>(primary.size()));
    const SIZE shortcutSize = MeasureText(dc, shortcut.data(),
        static_cast<int>(shortcut.size()));
    if (oldFont)
        SelectObject(dc, oldFont);

    const int shortcutGap = shortcut.empty() ? 0 : metrics.textGap * 3;
    const int arrowWidth = item.hasSubmenu ? metrics.arrowColumnWidth : 0;
    const int contentWidth = metrics.leftPadding +
        metrics.iconColumnWidth + metrics.textGap + primarySize.cx +
        shortcutGap + shortcutSize.cx + arrowWidth + metrics.rightPadding;
    return {
        static_cast<LONG>(std::max(metrics.minimumWidth, contentWidth)),
        static_cast<LONG>(metrics.rowHeight),
    };
}

bool DrawItem(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const RECT& bounds, UINT itemState,
    const Palette& palette, const Metrics& metrics)
{
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return false;

    FillSolidRect(dc, bounds, palette.background);
    if (item.separator)
    {
        const int centerY = (bounds.top + bounds.bottom) / 2;
        RECT line{
            bounds.left + metrics.outerInset * 2,
            centerY,
            bounds.right - metrics.outerInset * 2,
            centerY + 1,
        };
        FillSolidRect(dc, line, palette.separator);
        return true;
    }

    const bool disabled =
        (itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (itemState & ODS_SELECTED) != 0;
    if (selected && !disabled)
    {
        RECT selection = bounds;
        selection.left += metrics.outerInset;
        selection.right -= metrics.outerInset;
        selection.top += metrics.selectionInsetY;
        selection.bottom -= metrics.selectionInsetY;
        FillRoundedRect(dc, selection, metrics.selectionRadius,
            palette.hoverBackground);
    }

    const COLORREF foreground = disabled
        ? palette.disabledText : palette.text;
    const int oldBackgroundMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, foreground);

    if (item.checked || (itemState & ODS_CHECKED) != 0)
    {
        DrawCheckmark(dc, bounds, metrics, foreground);
    }
    else if (item.glyph && *item.glyph)
    {
        HGDIOBJ oldFont = SelectObject(dc,
            iconFont ? static_cast<HGDIOBJ>(iconFont)
                     : GetStockObject(DEFAULT_GUI_FONT));
        RECT iconBounds = bounds;
        iconBounds.left += metrics.leftPadding;
        iconBounds.right = iconBounds.left + metrics.iconColumnWidth;
        DrawTextW(dc, item.glyph, -1, &iconBounds,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (oldFont)
            SelectObject(dc, oldFont);
    }

    HGDIOBJ oldFont = SelectObject(dc,
        textFont ? static_cast<HGDIOBJ>(textFont)
                 : GetStockObject(DEFAULT_GUI_FONT));
    RECT textBounds = bounds;
    textBounds.left += metrics.leftPadding +
        metrics.iconColumnWidth + metrics.textGap;
    textBounds.right -= metrics.rightPadding +
        (item.hasSubmenu ? metrics.arrowColumnWidth : 0);

    std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    std::wstring primary(label.substr(0, tab));
    DrawTextW(dc, primary.c_str(), static_cast<int>(primary.size()),
        &textBounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
            DT_END_ELLIPSIS | DT_HIDEPREFIX);
    if (tab != std::wstring_view::npos)
    {
        std::wstring shortcut(label.substr(tab + 1));
        DrawTextW(dc, shortcut.c_str(), static_cast<int>(shortcut.size()),
            &textBounds, DT_RIGHT | DT_VCENTER | DT_SINGLELINE |
                DT_END_ELLIPSIS | DT_HIDEPREFIX);
    }
    if (oldFont)
        SelectObject(dc, oldFont);

    if (item.hasSubmenu)
        DrawSubmenuArrow(dc, bounds, metrics, foreground);

    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBackgroundMode);
    return true;
}

} // namespace snowdesktop::menu_icon
