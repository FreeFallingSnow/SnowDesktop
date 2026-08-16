#pragma once

#include "menu_quick_icon.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace snowdesktop::menu_icon
{

struct Palette
{
    COLORREF background = RGB(250, 250, 250);
    COLORREF hoverBackground = RGB(232, 232, 232);
    COLORREF text = RGB(26, 26, 26);
    COLORREF disabledText = RGB(118, 118, 118);
    COLORREF separator = RGB(225, 225, 225);
    COLORREF accent = RGB(0, 120, 212);
};

struct Metrics
{
    int rowHeight = 32;
    int separatorHeight = 8;
    int minimumWidth = 192;
    int outerInset = 4;
    int selectionInsetY = 2;
    int selectionRadius = 4;
    int leftPadding = 10;
    int iconColumnWidth = 22;
    int textGap = 7;
    int rightPadding = 9;
    int arrowColumnWidth = 16;
    int submenuArrowFontHeight = 16;
    int quickActionHeight = 52;
    int quickActionMinimumWidth = 46;
    int quickActionMaximumWidth = 64;
    int quickActionIconHeight = 20;
    int quickActionLabelGap = 1;
    int textFontHeight = 13;
    int quickActionTextFontHeight = 12;
    int iconFontHeight = 18;
    int quickActionFontHeight = 18;
};

struct ItemView
{
    const wchar_t* label = L"";
    const wchar_t* glyph = L"";
    bool separator = false;
    bool hasSubmenu = false;
    bool checked = false;
    MenuQuickIcon semanticIcon = MenuQuickIcon::FontGlyph;
    /** Optional non-owning premultiplied package image for the icon column. */
    HBITMAP image = nullptr;
};

struct ImageSourceView
{
    const std::uint8_t* pixels = nullptr;
    std::size_t bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};

struct TextInputView
{
    const wchar_t* text = L"";
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    const wchar_t* compositionText = L"";
    size_t compositionCursor = 0;
    bool focused = false;
    bool caretVisible = false;
};

/** @brief 返回接近 Windows 11 原生上下文菜单的明暗配色。 */
Palette ResolvePalette(bool lightTheme);

/** @brief 返回按显示器 DPI 缩放的菜单尺寸。 */
Metrics ResolveMetrics(UINT dpi);

/** Build a bounded square premultiplied bitmap for a menu image. */
HBITMAP CreateImageBitmap(const ImageSourceView& source, int pixelSize);

/** @brief 测量完整 owner-draw 菜单项。 */
SIZE MeasureItem(HDC dc, HFONT textFont, const ItemView& item,
    const Metrics& metrics);

/** @brief 绘制完整菜单项，包括背景、文字、图标、勾选和子菜单箭头。 */
bool DrawItem(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const RECT& bounds, UINT itemState,
    const Palette& palette, const Metrics& metrics,
    HFONT submenuArrowFont = nullptr);

/** @brief 绘制顶部快捷操作按钮；标签由菜单项用于键盘和辅助说明。 */
bool DrawQuickAction(HDC dc, HFONT textFont, HFONT iconFont,
    MenuQuickIcon quickIcon, const ItemView& item, const RECT& bounds,
    UINT itemState, const Palette& palette, const Metrics& metrics);

/** @brief 绘制位于普通菜单流中的紧凑横向操作按钮。 */
bool DrawInlineAction(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const RECT& bounds, UINT itemState,
    const Palette& palette, const Metrics& metrics);

/** @brief 绘制菜单内的单行文本搜索框。 */
bool DrawTextInput(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const TextInputView& input,
    const RECT& bounds, const Palette& palette, const Metrics& metrics);

} // namespace snowdesktop::menu_icon
