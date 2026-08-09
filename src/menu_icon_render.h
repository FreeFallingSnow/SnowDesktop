#pragma once

#include "menu_quick_icon.h"

#include <d2d1_1.h>
#include <dwrite.h>
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
    int quickActionHeight = 52;
    int quickActionMinimumWidth = 46;
    int quickActionMaximumWidth = 64;
    int quickActionIconHeight = 20;
    int quickActionLabelGap = 1;
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

/**
 * @brief 一次 D2D 绘制会话的共享上下文。
 *
 * dc 是当前绑定的渲染目标；文本格式均为 DIP 单位，绘制时按目标 DPI
 * 自动缩放。brush 由调用方持有并跨调用复用（每次绘制前 SetColor）。
 */
struct RenderContext
{
    ID2D1RenderTarget* dc = nullptr;
    IDWriteFactory* writeFactory = nullptr;
    IDWriteTextFormat* textFormat = nullptr;
    IDWriteTextFormat* quickTextFormat = nullptr;
    IDWriteTextFormat* iconFormat = nullptr;
    IDWriteTextFormat* quickIconFormat = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    const Palette* palette = nullptr;
    const Metrics* metrics = nullptr;
    /** 面板背景不透明度（blur 模式半透明）。 */
    float backgroundAlpha = 246.0f / 255.0f;
    /** 悬停选中背景不透明度。 */
    float hoverAlpha = 246.0f / 255.0f;
    /** 文字/图标等内容的最终不透明度。 */
    float contentAlpha = 246.0f / 255.0f;
};

/** @brief 返回接近 Windows 11 原生上下文菜单的明暗配色。 */
Palette ResolvePalette(bool lightTheme);

/** @brief 返回按显示器 DPI 缩放的菜单尺寸。 */
Metrics ResolveMetrics(UINT dpi);

/** @brief 使用 DWrite 测量菜单项内容宽度（不含 minWidth 下限）。 */
float MeasureItemWidth(IDWriteFactory* factory, IDWriteTextFormat* format,
    const ItemView& item, const Metrics& metrics);

/** @brief 绘制完整菜单项，包括背景、文字、图标、勾选和子菜单箭头。 */
bool DrawItem(RenderContext& ctx, const ItemView& item,
    const D2D1_RECT_F& bounds, UINT itemState);

/** @brief 绘制顶部快捷操作按钮。 */
bool DrawQuickAction(RenderContext& ctx, MenuQuickIcon quickIcon,
    const ItemView& item, const D2D1_RECT_F& bounds, UINT itemState);

/** @brief 绘制位于普通菜单流中的紧凑横向操作按钮。 */
bool DrawInlineAction(RenderContext& ctx, const ItemView& item,
    const D2D1_RECT_F& bounds, UINT itemState);

/** @brief 绘制菜单内的单行文本搜索框。 */
bool DrawTextInput(RenderContext& ctx, const ItemView& item,
    const TextInputView& input, const D2D1_RECT_F& bounds);

} // namespace snowdesktop::menu_icon
