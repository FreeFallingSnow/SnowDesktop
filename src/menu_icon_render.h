#pragma once

#include <windows.h>

namespace snowdesktop::menu_icon
{

struct Palette
{
    COLORREF background = RGB(250, 250, 250);
    COLORREF hoverBackground = RGB(232, 232, 232);
    COLORREF text = RGB(26, 26, 26);
    COLORREF disabledText = RGB(118, 118, 118);
    COLORREF separator = RGB(225, 225, 225);
};

struct Metrics
{
    int rowHeight = 34;
    int separatorHeight = 9;
    int minimumWidth = 200;
    int outerInset = 4;
    int selectionInsetY = 2;
    int selectionRadius = 4;
    int leftPadding = 10;
    int iconColumnWidth = 22;
    int textGap = 8;
    int rightPadding = 10;
    int arrowColumnWidth = 18;
};

struct ItemView
{
    const wchar_t* label = L"";
    const wchar_t* glyph = L"";
    bool separator = false;
    bool hasSubmenu = false;
    bool checked = false;
};

/** @brief 返回接近 Windows 11 原生上下文菜单的明暗配色。 */
Palette ResolvePalette(bool lightTheme);

/** @brief 返回按显示器 DPI 缩放的菜单尺寸。 */
Metrics ResolveMetrics(UINT dpi);

/** @brief 测量完整 owner-draw 菜单项。 */
SIZE MeasureItem(HDC dc, HFONT textFont, const ItemView& item,
    const Metrics& metrics);

/** @brief 绘制完整菜单项，包括背景、文字、图标、勾选和子菜单箭头。 */
bool DrawItem(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const RECT& bounds, UINT itemState,
    const Palette& palette, const Metrics& metrics);

} // namespace snowdesktop::menu_icon
