#pragma once

#include "menu_quick_icon.h"
#include "menu_builtin_icon.h"

#include <windows.h>
#include <algorithm>
#include <cmath>

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
    bool lightTheme = true;
    bool colorIcons = true;
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
    // Zero collapses the ordinary-row icon gutter without changing shortcuts.
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
    // Zero preserves the existing image bounds; compact menus cap package art.
    int maximumImageSize = 0;
    // Legacy menu bitmaps up to 16px use one DPI factor. Larger artwork
    // already has its own raster size and is only reduced when it cannot fit.
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
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
    BuiltinIcon builtinIcon = BuiltinIcon::None;
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
Metrics ResolveMetrics(UINT dpi, bool win10Style = false);

/** Build a bounded square premultiplied bitmap for a menu image. */
inline HBITMAP CreateImageBitmap(const ImageSourceView& source, int pixelSize)
{
    if (!source.pixels || pixelSize < 1 || pixelSize > 512 ||
        source.width == 0 || source.height == 0)
        return nullptr;
    const std::uint64_t rowBytes =
        static_cast<std::uint64_t>(source.width) * 4;
    const std::uint64_t requiredBytes =
        static_cast<std::uint64_t>(source.height - 1) * source.stride +
        rowBytes;
    if (rowBytes > source.stride || requiredBytes > source.bytes)
        return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = pixelSize;
    info.bmiHeader.biHeight = -pixelSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info,
        DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    if (!bitmap || !rawPixels)
    {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    auto* target = static_cast<std::uint8_t*>(rawPixels);
    std::fill_n(target,
        static_cast<std::size_t>(pixelSize) * pixelSize * 4,
        std::uint8_t{ 0 });

    const double scale = std::min(
        static_cast<double>(pixelSize) / source.width,
        static_cast<double>(pixelSize) / source.height);
    const int drawWidth = std::clamp(
        static_cast<int>(std::lround(source.width * scale)), 1, pixelSize);
    const int drawHeight = std::clamp(
        static_cast<int>(std::lround(source.height * scale)), 1, pixelSize);
    const int offsetX = (pixelSize - drawWidth) / 2;
    const int offsetY = (pixelSize - drawHeight) / 2;
    for (int y = 0; y < drawHeight; ++y)
    {
        const double sampleY = std::clamp(
            (static_cast<double>(y) + 0.5) * source.height / drawHeight -
                0.5,
            0.0, static_cast<double>(source.height - 1));
        const auto y0 = static_cast<std::uint32_t>(std::floor(sampleY));
        const auto y1 = std::min(y0 + 1, source.height - 1);
        const double fy = sampleY - y0;
        for (int x = 0; x < drawWidth; ++x)
        {
            const double sampleX = std::clamp(
                (static_cast<double>(x) + 0.5) * source.width / drawWidth -
                    0.5,
                0.0, static_cast<double>(source.width - 1));
            const auto x0 = static_cast<std::uint32_t>(std::floor(sampleX));
            const auto x1 = std::min(x0 + 1, source.width - 1);
            const double fx = sampleX - x0;
            const std::uint8_t* p00 = source.pixels +
                static_cast<std::size_t>(y0) * source.stride + x0 * 4;
            const std::uint8_t* p10 = source.pixels +
                static_cast<std::size_t>(y0) * source.stride + x1 * 4;
            const std::uint8_t* p01 = source.pixels +
                static_cast<std::size_t>(y1) * source.stride + x0 * 4;
            const std::uint8_t* p11 = source.pixels +
                static_cast<std::size_t>(y1) * source.stride + x1 * 4;
            std::uint8_t* destination = target +
                (static_cast<std::size_t>(offsetY + y) * pixelSize +
                    offsetX + x) * 4;
            for (int channel = 0; channel < 4; ++channel)
            {
                const double top = p00[channel] +
                    (p10[channel] - p00[channel]) * fx;
                const double bottom = p01[channel] +
                    (p11[channel] - p01[channel]) * fx;
                destination[channel] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(top + (bottom - top) * fy),
                        0l, 255l));
            }
        }
    }
    return bitmap;
}

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

enum class InlineActionStyle { Plain, Secondary, Primary };

/** @brief 绘制位于普通菜单流中的紧凑横向操作按钮。 */
bool DrawInlineAction(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const RECT& bounds, UINT itemState,
    const Palette& palette, const Metrics& metrics,
    InlineActionStyle style = InlineActionStyle::Plain);

/** Shared text origin for painting, pointer caret placement and IME anchors. */
RECT TextInputTextBounds(const RECT& bounds, const Metrics& metrics, bool hasIcon);

/** @brief 绘制菜单内的单行文本搜索框。 */
bool DrawTextInput(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const TextInputView& input,
    const RECT& bounds, const Palette& palette, const Metrics& metrics);

} // namespace snowdesktop::menu_icon
