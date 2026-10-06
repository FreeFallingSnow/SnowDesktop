#pragma once
#include <windows.h>
#include <algorithm>

namespace snowdesktop::modern_menu::scroll_hint
{
// The layout removes a reached edge's band; drawing likewise omits its arrow
// and separator instead of suggesting a direction with no remaining content.
inline void Draw(HDC dc, RECT band, bool top, int offset, int maximum, bool hovered,
                 UINT dpi, COLORREF background, COLORREF hoverBackground,
                 COLORREF separatorColor, COLORREF arrowColor)
{
    if (top ? offset <= 0 : offset >= maximum) return;
    const auto scale = [dpi](int value) { return std::max(1, MulDiv(value, dpi, 96)); };
    HBRUSH brush = CreateSolidBrush(hovered ? hoverBackground : background);
    FillRect(dc, &band, brush); DeleteObject(brush);
    HPEN separator = CreatePen(PS_SOLID, scale(1), separatorColor);
    HGDIOBJ oldPen = SelectObject(dc, separator);
    const int edge = top ? band.bottom - 1 : band.top;
    MoveToEx(dc, band.left + scale(6), edge, nullptr);
    LineTo(dc, band.right - scale(6), edge);
    SelectObject(dc, oldPen); DeleteObject(separator);
    const int centerX = (band.left + band.right) / 2;
    const int centerY = (band.top + band.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, scale(2), arrowColor);
    oldPen = SelectObject(dc, pen);
    const int half = scale(4), rise = scale(2);
    MoveToEx(dc, centerX - half, centerY + (top ? rise : -rise), nullptr);
    LineTo(dc, centerX, centerY + (top ? -rise : rise));
    LineTo(dc, centerX + half, centerY + (top ? rise : -rise));
    SelectObject(dc, oldPen); DeleteObject(pen);
}
}
