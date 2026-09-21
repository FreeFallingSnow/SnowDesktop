#include "menu_icon_render.h"
#include "modern_menu_scroll_hint.h"
#include "menu_fluent_glyphs.h"
#include "resource.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace
{

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::uint32_t PixelColor(COLORREF color)
{
    return static_cast<std::uint32_t>(GetBValue(color)) |
        (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
        (static_cast<std::uint32_t>(GetRValue(color)) << 16);
}

int CountColor(const std::uint32_t* pixels, int count, COLORREF color)
{
    const std::uint32_t expected = PixelColor(color);
    int matches = 0;
    for (int i = 0; i < count; ++i)
    {
        if ((pixels[i] & 0x00FFFFFFu) == expected)
            ++matches;
    }
    return matches;
}

int CountBlueAccentPixels(const std::uint32_t* pixels, int count)
{
    int matches = 0;
    for (int i = 0; i < count; ++i)
    {
        const unsigned blue = pixels[i] & 0xFFu;
        const unsigned green = (pixels[i] >> 8) & 0xFFu;
        const unsigned red = (pixels[i] >> 16) & 0xFFu;
        if (blue > red + 35 && green > red + 20)
            ++matches;
    }
    return matches;
}

int CountColorInRect(const std::uint32_t* pixels, int width, int height,
    const RECT& bounds, COLORREF color)
{
    const int left = std::clamp(static_cast<int>(bounds.left), 0, width);
    const int top = std::clamp(static_cast<int>(bounds.top), 0, height);
    const int right = std::clamp(
        static_cast<int>(bounds.right), left, width);
    const int bottom = std::clamp(
        static_cast<int>(bounds.bottom), top, height);
    const std::uint32_t expected = PixelColor(color);
    int matches = 0;
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            if ((pixels[y * width + x] & 0x00FFFFFFu) == expected)
                ++matches;
        }
    }
    return matches;
}

int CountBlueAccentPixelsInRect(const std::uint32_t* pixels,
    int width, int height, const RECT& bounds)
{
    const int left = std::clamp(static_cast<int>(bounds.left), 0, width);
    const int top = std::clamp(static_cast<int>(bounds.top), 0, height);
    const int right = std::clamp(
        static_cast<int>(bounds.right), left, width);
    const int bottom = std::clamp(
        static_cast<int>(bounds.bottom), top, height);
    int matches = 0;
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            const std::uint32_t pixel = pixels[y * width + x];
            const unsigned blue = pixel & 0xFFu;
            const unsigned green = (pixel >> 8) & 0xFFu;
            const unsigned red = (pixel >> 16) & 0xFFu;
            if (blue > red + 35 && green > red + 20)
                ++matches;
        }
    }
    return matches;
}

int CountPixelsDifferentFromColorInRect(const std::uint32_t* pixels,
    int width, int height, const RECT& bounds, COLORREF color)
{
    const int left = std::clamp(static_cast<int>(bounds.left), 0, width);
    const int top = std::clamp(static_cast<int>(bounds.top), 0, height);
    const int right = std::clamp(
        static_cast<int>(bounds.right), left, width);
    const int bottom = std::clamp(
        static_cast<int>(bounds.bottom), top, height);
    const std::uint32_t expected = PixelColor(color);
    int matches = 0;
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            if ((pixels[y * width + x] & 0x00FFFFFFu) != expected)
                ++matches;
        }
    }
    return matches;
}

RECT FindPixelsDifferentFromColorInRect(const std::uint32_t* pixels,
    int width, int height, const RECT& bounds, COLORREF color)
{
    const int left = std::clamp(static_cast<int>(bounds.left), 0, width);
    const int top = std::clamp(static_cast<int>(bounds.top), 0, height);
    const int right = std::clamp(
        static_cast<int>(bounds.right), left, width);
    const int bottom = std::clamp(
        static_cast<int>(bounds.bottom), top, height);
    const std::uint32_t expected = PixelColor(color);
    RECT result{ right, bottom, left, top };
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            if ((pixels[y * width + x] & 0x00FFFFFFu) == expected)
                continue;
            result.left = std::min<LONG>(result.left, x);
            result.top = std::min<LONG>(result.top, y);
            result.right = std::max<LONG>(result.right, x + 1);
            result.bottom = std::max<LONG>(result.bottom, y + 1);
        }
    }
    return result;
}

bool SavePreviewBitmap(const std::filesystem::path& path,
    const BITMAPINFOHEADER& info, const void* pixels, DWORD pixelBytes)
{
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(info);
    fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(reinterpret_cast<const char*>(&fileHeader),
        sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(static_cast<const char*>(pixels), pixelBytes);
    return output.good();
}

void CheckCompactRendering()
{
    using namespace snowdesktop::menu_icon;
    constexpr int width = 768;
    constexpr int height = 80;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    Expect(bitmap && raw && dc, "compact raster fixture is available");
    auto* pixels = static_cast<std::uint32_t*>(raw);
    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    std::array<std::uint32_t, 32 * 32> imagePixels;
    imagePixels.fill(0xffff0000u);
    const ImageSourceView imageSource{
        reinterpret_cast<const std::uint8_t*>(imagePixels.data()),
        sizeof(imagePixels), 32, 32, 32 * 4};
    HBITMAP image = CreateImageBitmap(imageSource, 32);
    Expect(image != nullptr, "oversized package image fixture is decoded");
    const UINT dpis[] = {96, 120, 144, 192};
    const int rowHeights[] = {24, 30, 36, 48};
    const int iconSizes[] = {16, 20, 24, 32};
    for (size_t i = 0; i < std::size(dpis); ++i)
    {
        const auto metrics = ResolveMetrics(dpis[i], true);
        Expect(metrics.rowHeight == rowHeights[i] &&
                metrics.iconFontHeight == iconSizes[i],
            "Win10 menu density follows the specified sizes at common DPI scales");
        HFONT textFont = CreateFontW(-metrics.textFontHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT iconFont = CreateFontW(-metrics.iconFontHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"FluentSystemIcons-Regular");
        Expect(textFont && iconFont, "compact text and icon fonts are created");
        const RECT row{0, 0, width, rowHeights[i]};
        for (const bool light : {true, false})
        {
            const auto palette = ResolvePalette(light);
            // The actual hint renderer must leave a reached edge completely
            // blank, including its separator. Middle positions keep both hints.
            const RECT band{0, 0, 200, MulDiv(18, dpis[i], 96)};
            for (const int offset : {0, 50, 100}) for (const bool top : {true, false})
            {
                std::fill_n(pixels, width * height, PixelColor(palette.background));
                snowdesktop::modern_menu::scroll_hint::Draw(dc, band, top, offset, 100, false,
                    dpis[i], palette.background, palette.hoverBackground, palette.separator, palette.text);
                GdiFlush();
                const auto ink = CountPixelsDifferentFromColorInRect(pixels, width, height, band, palette.background);
                Expect((ink > 0) == (top ? offset != 0 : offset != 100),
                    "only directions with remaining content paint a scroll hint at every DPI and theme");
            }
            ItemView item{L"复制一个较长名称的项目\tCtrl+C", L"\uF32B"};
            const auto measured = MeasureItem(dc, textFont, item, metrics);
            Expect(measured.cy == rowHeights[i] && measured.cx < width,
                "compact rows measure both localized text and shortcuts");
            Expect(DrawItem(dc, textFont, iconFont, item, row, 0, palette, metrics),
                "compact glyph and text rows render in both palettes");
            const RECT iconColumn{metrics.leftPadding, 0,
                metrics.leftPadding + metrics.iconColumnWidth, row.bottom};
            const auto iconInk = FindPixelsDifferentFromColorInRect(
                pixels, width, height, iconColumn, palette.background);
            Expect(iconInk.right > iconInk.left && iconInk.top > 0 &&
                    iconInk.bottom < row.bottom,
                "small Fluent glyphs remain visible with vertical breathing room");
            const RECT textColumn{iconColumn.right + metrics.textGap, 0,
                width - metrics.rightPadding, row.bottom};
            const auto textInk = FindPixelsDifferentFromColorInRect(
                pixels, width, height, textColumn, palette.background);
            Expect(textInk.right > textInk.left && textInk.top > 0 &&
                    textInk.bottom < row.bottom,
                "compact CJK text and shortcuts fit inside the row");

            item.label = L"";
            item.glyph = L"";
            item.image = image;
            DrawItem(dc, textFont, iconFont, item, row, 0, palette, metrics);
            const auto imageInk = FindPixelsDifferentFromColorInRect(
                pixels, width, height, iconColumn, palette.background);
            Expect(imageInk.right - imageInk.left == iconSizes[i] &&
                    imageInk.bottom - imageInk.top == iconSizes[i],
                "package images are bounded to the same small size as font icons");
            item.image = nullptr;
            item.checked = true;
            DrawItem(dc, textFont, iconFont, item, row, 0, palette, metrics);
            Expect(CountPixelsDifferentFromColorInRect(
                    pixels, width, height, iconColumn, palette.background) > 0,
                "compact checked rows retain a visible checkmark");
            item.checked = false;
            DrawItem(dc, textFont, iconFont, item, row, ODS_SELECTED, palette, metrics);
            Expect(CountColorInRect(pixels, width, height, row, palette.hoverBackground) > 0,
                "compact selection has a visible highlight");
            DrawItem(dc, textFont, iconFont, item, row,
                ODS_DISABLED | ODS_SELECTED, palette, metrics);
            Expect(CountColorInRect(pixels, width, height, row, palette.hoverBackground) == 0,
                "disabled compact commands cannot acquire a selected background");
            const TextInputView input{L"搜索组件", 4, 4, L"", 0, true, true};
            item.glyph = L"\uF68F";
            Expect(DrawTextInput(dc, textFont, iconFont, item, input, row, palette, metrics),
                "compact search input renders with its caret and localized text");
        }
        DeleteObject(iconFont);
        DeleteObject(textFont);
    }
    DeleteObject(image);
    SelectObject(dc, previousBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
}

void CheckIconlessRows(HDC dc, std::uint32_t* pixels, int width, int height)
{
    using namespace snowdesktop::menu_icon;
    for (const bool compact : {false, true}) for (const UINT dpi : {96u, 120u, 144u, 192u})
    {
        const auto withIcons = ResolveMetrics(dpi, compact);
        auto withoutIcons = withIcons;
        withoutIcons.iconColumnWidth = 0;
        HFONT font = CreateFontW(-withIcons.textFontHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        Expect(font != nullptr, "iconless row font is available");
        const int gutter = withIcons.iconColumnWidth + withIcons.textGap;
        ItemView item{L"A long menu command that exceeds the minimum menu width\tCtrl+Shift+M", L""};
        const auto normal = MeasureItem(dc, font, item, withIcons);
        const auto narrow = MeasureItem(dc, font, item, withoutIcons);
        Expect(normal.cx - narrow.cx == gutter && normal.cy == narrow.cy,
            "iconless measurement reclaims only the icon gutter, preserving shortcut spacing and row height");
        item.label = L"Menu";
        const RECT bounds{0, 0, width, withIcons.rowHeight};
        for (const bool light : {true, false})
        {
            const auto palette = ResolvePalette(light);
            DrawItem(dc, font, nullptr, item, bounds, 0, palette, withIcons);
            GdiFlush();
            const auto before = FindPixelsDifferentFromColorInRect(pixels, width, height, bounds, palette.background);
            DrawItem(dc, font, nullptr, item, bounds, 0, palette, withoutIcons);
            GdiFlush();
            const auto after = FindPixelsDifferentFromColorInRect(pixels, width, height, bounds, palette.background);
            Expect(before.right > before.left && after.right > after.left && before.left - after.left == gutter &&
                    before.top == after.top && before.bottom == after.bottom,
                "iconless text paints at the reclaimed inset at every DPI and theme");
        }
        DeleteObject(font);
    }
}

// Real embedded resources and production WIC/GDI entry points: catches missing
// packaging, wrong theme/down-arrow coloring, filled holes, and skipped draw paths.
void CheckBuiltinArtwork(HDC dc, HFONT font, HFONT iconFont,
    std::uint32_t* pixels, int width, int height)
{
    using namespace snowdesktop::menu_icon;
    struct Sample { BuiltinIcon icon; double x; double y; };
    const Sample holes[] = {
        { BuiltinIcon::Display, 8.75, 17.75 },
        { BuiltinIcon::Display, 15.25, 6.25 },
        { BuiltinIcon::Paste, 15.5, 15 },
        { BuiltinIcon::Pin, 13.5, 9 },
        { BuiltinIcon::Settings, 12, 12 },
    };
    const Sample blue[] = {
        { BuiltinIcon::Display, 15.25, 3.5 },
        { BuiltinIcon::Display, 8.75, 15 },
        { BuiltinIcon::Paste, 20.25, 15 },
        { BuiltinIcon::Settings, 12, 9 },
        { BuiltinIcon::Sort, 17.25, 10 },
        { BuiltinIcon::Sort, 14.25, 16.25 },
        { BuiltinIcon::Sort, 20.25, 16.25 },
    };
    const Sample neutral[] = {
        { BuiltinIcon::Display, 3, 6.25 },
        { BuiltinIcon::Paste, 3.75, 10 },
        { BuiltinIcon::Pin, 5, 19 },
        { BuiltinIcon::Pin, 17.5, 5.25 },
        { BuiltinIcon::AddPage, 12, 12 },
        { BuiltinIcon::Sort, 6.75, 12 },
        { BuiltinIcon::Sort, 3.75, 7.75 },
        { BuiltinIcon::Sort, 9.75, 7.75 },
    };
    for (const bool light : { true, false })
    {
        for (int value = static_cast<int>(BuiltinIcon::Sort);
            value <= static_cast<int>(BuiltinIcon::Workshop); ++value)
        {
            const auto icon = static_cast<BuiltinIcon>(value);
            HBITMAP image = CreateBuiltinIconBitmap(icon, light, 128);
            BITMAP bitmap{};
            Expect(image && GetObjectW(image, sizeof(bitmap), &bitmap) && bitmap.bmBits,
                "all built-in menu artwork is embedded and decodes to a DIB");
            const auto* art = static_cast<const std::uint32_t*>(bitmap.bmBits);
            const auto sample = [&](const Sample& point) {
                return art[static_cast<int>(point.y * 128 / 24) * 128 +
                    static_cast<int>(point.x * 128 / 24)];
            };
            for (const auto& point : holes) if (point.icon == icon)
                Expect((sample(point) >> 24) == 0,
                    "approved display/paste/pin/settings interiors stay transparent");
            const auto matches = [&](std::uint32_t pixel, COLORREF color) {
                const auto expected = PixelColor(color);
                if ((pixel >> 24) < 240) return false;
                for (int shift : { 0, 8, 16 })
                    if (std::abs(static_cast<int>((pixel >> shift) & 255) -
                        static_cast<int>((expected >> shift) & 255)) > 2) return false;
                return true;
            };
            for (const auto& point : blue) if (point.icon == icon)
                Expect(matches(sample(point), light ? RGB(0, 120, 212) : RGB(96, 205, 255)),
                    "requested contours and complete down arrow use the theme blue");
            for (const auto& point : neutral) if (point.icon == icon)
                Expect(matches(sample(point), light ? RGB(48, 52, 59) : RGB(228, 230, 234)),
                    "up arrow, clipboard back, entire pin, add-page plus and rails stay neutral");
            DeleteObject(image);
        }
        for (const bool compact : { false, true })
        for (const UINT dpi : { 96u, 120u, 144u, 192u })
        {
            const auto metrics = ResolveMetrics(dpi, compact);
            auto palette = ResolvePalette(light);
            palette.colorIcons = true; // Deterministic, regardless of the test machine theme.
            const RECT row{ 0, 0, width, metrics.rowHeight };
            const RECT column{ metrics.leftPadding, 0,
                metrics.leftPadding + metrics.iconColumnWidth, metrics.rowHeight };
            ItemView item{ L"", snowdesktop::menu_fluent_glyphs::kSort };
            item.builtinIcon = BuiltinIcon::Sort;
            DrawItem(dc, font, iconFont, item, row, 0, palette, metrics);
            GdiFlush();
            const int enabledBlue = CountBlueAccentPixelsInRect(pixels, width, height, column);
            Expect(enabledBlue > 0, "ordinary rows draw colored art at each supported DPI/style");
            RECT leftHalf = column;
            leftHalf.right = (column.left + column.right) / 2;
            // The down arrow's left tip reaches x=12 of its 24px canvas;
            // downsampling can put its blue edge just across the midpoint.
            // Inspect the upper-left arrowhead, not that shared canvas border.
            leftHalf.bottom = metrics.rowHeight / 2;
            Expect(CountBlueAccentPixelsInRect(pixels, width, height, leftHalf) == 0,
                "small sorting icon keeps the left up arrow neutral");
            DrawItem(dc, font, iconFont, item, row, ODS_DISABLED, palette, metrics);
            GdiFlush();
            Expect(CountBlueAccentPixelsInRect(pixels, width, height, column) <= enabledBlue,
                "disabled art uses the existing reduced-opacity path");
            DrawItem(dc, font, iconFont, item, row, ODS_CHECKED, palette, metrics);
            GdiFlush();
            Expect(CountBlueAccentPixelsInRect(pixels, width, height, column) == 0,
                "checked menu state takes precedence over colored art");
            palette.colorIcons = false;
            DrawItem(dc, font, iconFont, item, row, 0, palette, metrics);
            GdiFlush();
            Expect(CountBlueAccentPixelsInRect(pixels, width, height, column) == 0 &&
                CountPixelsDifferentFromColorInRect(pixels, width, height, column, palette.background) > 0,
                "high-contrast color suppression retains a visible Fluent fallback");
            palette.colorIcons = true;
            item.builtinIcon = BuiltinIcon::Paste;
            const RECT quick{ 0, 0, metrics.quickActionMaximumWidth, metrics.quickActionHeight };
            DrawQuickAction(dc, font, iconFont, snowdesktop::MenuQuickIcon::Paste,
                item, quick, 0, palette, metrics);
            GdiFlush();
            Expect(CountBlueAccentPixelsInRect(pixels, width, height, quick) > 0,
                "top paste button draws approved blue outline");
            item.builtinIcon = BuiltinIcon::Search;
            DrawInlineAction(dc, font, iconFont, item, row, 0, palette, metrics);
            GdiFlush();
            Expect(CountBlueAccentPixelsInRect(pixels, width, height, row) > 0,
                "inline widget actions accept built-in colored art");
            DrawTextInput(dc, font, iconFont, item, {}, row, palette, metrics);
            GdiFlush();
            Expect(CountBlueAccentPixelsInRect(pixels, width, height, row) > 0,
                "widget search input draws colored artwork");
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HRSRC fontResource = FindResourceW(instance,
        MAKEINTRESOURCEW(IDR_FLUENT_REGULAR_FONT), RT_RCDATA);
    HGLOBAL fontResourceHandle = fontResource
        ? LoadResource(instance, fontResource) : nullptr;
    void* fontData = fontResourceHandle
        ? LockResource(fontResourceHandle) : nullptr;
    const DWORD fontDataSize = fontResource
        ? SizeofResource(instance, fontResource) : 0;
    DWORD fontCount = 0;
    HANDLE fluentFontHandle = fontData && fontDataSize > 0
        ? AddFontMemResourceEx(fontData, fontDataSize, nullptr, &fontCount)
        : nullptr;
    Expect(fluentFontHandle != nullptr && fontCount > 0,
        "embedded Fluent icon font is loaded for rendering");
    CheckCompactRendering();

    const auto light = snowdesktop::menu_icon::ResolvePalette(true);
    const auto dark = snowdesktop::menu_icon::ResolvePalette(false);
    Expect(light.background != light.disabledText,
        "light disabled text contrasts with the menu background");
    Expect(dark.background != dark.disabledText,
        "dark disabled text contrasts with the menu background");

    const auto metrics96 = snowdesktop::menu_icon::ResolveMetrics(96);
    const auto metrics120 = snowdesktop::menu_icon::ResolveMetrics(120);
    const auto metrics144 = snowdesktop::menu_icon::ResolveMetrics(144);
    const auto metrics192 = snowdesktop::menu_icon::ResolveMetrics(192);
    Expect(metrics96.rowHeight == 32,
        "menu uses the intended compact Win11 row height");
    Expect(metrics192.rowHeight == metrics96.rowHeight * 2,
        "menu row height follows monitor DPI");
    Expect(metrics96.quickActionHeight == 52,
        "quick actions reserve room for an icon and label");
    Expect(metrics96.quickActionMinimumWidth == 46 &&
            metrics96.quickActionMaximumWidth == 64,
        "quick actions stay compact even when a label is long");
    Expect(metrics192.quickActionHeight ==
            metrics96.quickActionHeight * 2,
        "quick-action labels follow monitor DPI");
    Expect(metrics192.quickActionMaximumWidth ==
            metrics96.quickActionMaximumWidth * 2,
        "quick-action width limit follows monitor DPI");
    Expect(metrics96.textFontHeight == 13 &&
            metrics96.quickActionTextFontHeight == 12,
        "96-DPI regular and quick-action text use their configured sizes");
    Expect(metrics96.submenuArrowFontHeight == 16 &&
            metrics120.submenuArrowFontHeight == 20 &&
            metrics144.submenuArrowFontHeight == 24 &&
            metrics192.submenuArrowFontHeight == 32,
        "Fluent submenu chevron font follows monitor DPI");
    Expect(metrics192.textFontHeight ==
            metrics96.textFontHeight * 2 &&
            metrics192.quickActionTextFontHeight ==
                metrics96.quickActionTextFontHeight * 2,
        "menu text sizes follow monitor DPI");
    Expect(metrics96.iconFontHeight == 18 &&
            metrics96.quickActionFontHeight == 18,
        "96-DPI Fluent icons use the compact menu size");
    Expect(metrics120.iconFontHeight == 23 &&
            metrics144.iconFontHeight == 27 &&
            metrics192.iconFontHeight == 36,
        "compact Fluent icons scale consistently across common DPIs");

    constexpr int kWidth = 240;
    constexpr int kHeight = 180;
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = kWidth;
    bitmapInfo.bmiHeader.biHeight = -kHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    bitmapInfo.bmiHeader.biSizeImage = kWidth * kHeight * 4;

    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo,
        DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    Expect(bitmap != nullptr && rawPixels != nullptr,
        "test render target is created");
    auto* pixels = static_cast<std::uint32_t*>(rawPixels);
    std::fill_n(pixels, kWidth * kHeight, 0u);

    HDC dc = CreateCompatibleDC(nullptr);
    Expect(dc != nullptr, "test device context is created");
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    Expect(oldBitmap != nullptr, "test bitmap is selected");
    HFONT font = CreateFontW(-metrics96.textFontHeight,
        0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    Expect(font != nullptr, "test font is created");
    HFONT fluentFont = CreateFontW(-metrics96.quickActionFontHeight,
        0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"FluentSystemIcons-Regular");
    Expect(fluentFont != nullptr, "test Fluent icon font is created");
    HFONT submenuArrowFont = CreateFontW(-metrics96.submenuArrowFontHeight,
        0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"FluentSystemIcons-Regular");
    Expect(submenuArrowFont != nullptr,
        "test Fluent submenu chevron font is created");

    CheckIconlessRows(dc, pixels, kWidth, kHeight);
    CheckBuiltinArtwork(dc, font, fluentFont, pixels, kWidth, kHeight);

    const snowdesktop::menu_icon::ItemView normal{
        L"Open", L"O", false, false, false,
    };
    const snowdesktop::menu_icon::ItemView disabled{
        L"Edit schedule", L"E", false, false, false,
    };
    const snowdesktop::menu_icon::ItemView submenu{
        L"View options", L"V", false, true, false,
    };
    const snowdesktop::menu_icon::ItemView separator{
        L"", L"", true, false, false,
    };

    const SIZE measured = snowdesktop::menu_icon::MeasureItem(
        dc, font, submenu, metrics96);
    Expect(measured.cx >= metrics96.minimumWidth,
        "menu rows retain the Win11 minimum width");
    Expect(measured.cy == metrics96.rowHeight,
        "regular menu rows use the configured height");

    const snowdesktop::menu_icon::ItemView arrowOnlySubmenu{
        L"", L"", false, true, false,
    };
    const RECT arrowOnlyBounds{ 0, 0, kWidth, metrics96.rowHeight };
    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawItem(dc, font, fluentFont,
        arrowOnlySubmenu, arrowOnlyBounds, 0, light, metrics96,
        submenuArrowFont),
        "submenu chevron renders for geometry checks");
    const RECT arrowColumn{
        kWidth - metrics96.rightPadding - metrics96.arrowColumnWidth,
        0,
        kWidth - metrics96.rightPadding,
        metrics96.rowHeight,
    };
    const RECT arrowInk = FindPixelsDifferentFromColorInRect(
        pixels, kWidth, kHeight, arrowColumn, light.background);
    const int arrowInkPixels = CountPixelsDifferentFromColorInRect(
        pixels, kWidth, kHeight, arrowColumn, light.background);
    const int solidArrowPixels = CountColorInRect(
        pixels, kWidth, kHeight, arrowColumn, light.text);
    Expect(arrowInkPixels > solidArrowPixels,
        "Fluent submenu chevron includes anti-aliased edge coverage");
    Expect(arrowInk.right - arrowInk.left >= 4 &&
            arrowInk.bottom - arrowInk.top >= 7,
        "96-DPI submenu chevron keeps a complete visible shape");
    const int arrowRowCenterTwice =
        arrowOnlyBounds.top + arrowOnlyBounds.bottom - 1;
    const int arrowInkCenterTwice =
        arrowInk.top + arrowInk.bottom - 1;
    Expect(std::abs(arrowRowCenterTwice - arrowInkCenterTwice) <= 1,
        "Fluent submenu chevron stays vertically centered");
    Expect(arrowInk.right < arrowOnlyBounds.right - metrics96.rightPadding,
        "submenu chevron keeps a safe inset from the row edge");

    for (const UINT alignmentDpi : std::array<UINT, 4>{ 96, 120, 144, 192 })
    {
        const auto alignmentMetrics =
            snowdesktop::menu_icon::ResolveMetrics(alignmentDpi);
        HFONT alignmentFont = CreateFontW(
            -alignmentMetrics.textFontHeight,
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        Expect(alignmentFont != nullptr,
            "DPI-specific menu text font is created");
        const RECT dpiBounds{
            0, 0, kWidth,
            std::min(kHeight, alignmentMetrics.rowHeight),
        };
        const RECT dpiTextBounds{
            alignmentMetrics.leftPadding + alignmentMetrics.iconColumnWidth +
                alignmentMetrics.textGap,
            dpiBounds.top,
            kWidth - alignmentMetrics.rightPadding,
            dpiBounds.bottom,
        };
        const std::array<const wchar_t*, 3> alignmentLabels{
            L"显示设置", L"Settings", L"Ag",
        };
        for (size_t alignmentIndex = 0;
             alignmentIndex < alignmentLabels.size(); ++alignmentIndex)
        {
            const wchar_t* alignmentLabel = alignmentLabels[alignmentIndex];
            const snowdesktop::menu_icon::ItemView dpiItem{
                alignmentLabel, L"", false, false, false,
            };
            std::fill_n(pixels, kWidth * kHeight, 0u);
            snowdesktop::menu_icon::DrawItem(dc, alignmentFont, fluentFont,
                dpiItem, dpiBounds, 0, light, alignmentMetrics);
            const RECT dpiInk = FindPixelsDifferentFromColorInRect(
                pixels, kWidth, kHeight, dpiTextBounds, light.background);
            Expect(dpiInk.right > dpiInk.left &&
                    dpiInk.bottom > dpiInk.top,
                "menu label produces visible ink for alignment checks");
            const int rowCenterTwice =
                dpiBounds.top + dpiBounds.bottom - 1;
            const int inkCenterTwice =
                dpiInk.top + dpiInk.bottom - 1;
            Expect(std::abs(rowCenterTwice - inkCenterTwice) <= 1,
                alignmentIndex == 0
                    ? "Chinese menu text stays vertically centered across DPI"
                    : "Latin menu text stays vertically centered across DPI");
        }
        DeleteObject(alignmentFont);
    }

    const RECT normalBounds{ 0, 0, kWidth, 34 };
    const RECT quickBounds{ 0, 111, kWidth, 167 };
    const RECT disabledBounds{ 0, 34, kWidth, 68 };
    const RECT selectedBounds{ 0, 68, kWidth, 102 };
    const RECT separatorBounds{ 0, 102, kWidth, 111 };
    Expect(snowdesktop::menu_icon::DrawQuickAction(dc, font, fluentFont,
        snowdesktop::MenuQuickIcon::Copy, normal, quickBounds,
        ODS_SELECTED, light, metrics96),
        "Win11-style quick action renders");
    Expect(snowdesktop::menu_icon::DrawItem(dc, font, font, normal,
        normalBounds, 0, light, metrics96),
        "normal owner-draw menu item renders");
    Expect(snowdesktop::menu_icon::DrawItem(dc, font, font, disabled,
        disabledBounds, ODS_DISABLED | ODS_GRAYED, light, metrics96),
        "disabled owner-draw menu item renders");
    Expect(snowdesktop::menu_icon::DrawItem(dc, font, font, submenu,
        selectedBounds, ODS_SELECTED, light, metrics96,
        submenuArrowFont),
        "hovered owner-draw menu item renders");
    Expect(snowdesktop::menu_icon::DrawItem(dc, font, font, separator,
        separatorBounds, 0, light, metrics96),
        "owner-draw separator renders");
    const int disabledColorPixels = CountColor(pixels,
        kWidth * kHeight, light.disabledText);
    const int hoverColorPixels = CountColor(pixels,
        kWidth * kHeight, light.hoverBackground);
    const int separatorColorPixels = CountColor(pixels,
        kWidth * kHeight, light.separator);
    const int accentColorPixels = CountBlueAccentPixels(
        pixels, kWidth * kHeight);
    Expect(disabledColorPixels > 0,
        "disabled text and icon remain visibly gray without hover");
    Expect(disabledColorPixels < metrics96.rowHeight *
        metrics96.iconColumnWidth,
        "disabled glyph retains its shape instead of becoming a block");
    Expect(hoverColorPixels > 0,
        "selected row has a rounded Win11-style hover fill");
    Expect(separatorColorPixels > 0,
        "separator uses the themed separator color");
    Expect(accentColorPixels > 0,
        "quick-action Fluent icon contains a blue accent layer");

    const std::array<std::uint8_t, 8> packageImagePixels{
        0, 0, 255, 255,
        255, 255, 0, 255,
    };
    const snowdesktop::menu_icon::ImageSourceView packageImageSource{
        packageImagePixels.data(), packageImagePixels.size(), 2, 1, 8,
    };
    HBITMAP packageImage = snowdesktop::menu_icon::CreateImageBitmap(
        packageImageSource, metrics96.iconFontHeight);
    BITMAP packageImageInfo{};
    Expect(packageImage &&
            GetObjectW(packageImage, sizeof(packageImageInfo),
                &packageImageInfo) != 0 &&
            packageImageInfo.bmWidth == metrics96.iconFontHeight &&
            packageImageInfo.bmHeight == metrics96.iconFontHeight,
        "package menu image is converted to a bounded DPI-sized bitmap");
    const snowdesktop::menu_icon::ItemView packageImageItem{
        L"Package icon", L"", false, false, false,
        snowdesktop::MenuQuickIcon::FontGlyph, packageImage,
    };
    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawItem(dc, font, fluentFont,
            packageImageItem, normalBounds, 0, light, metrics96),
        "package image menu item renders");
    const RECT packageImageColumn{
        metrics96.leftPadding, normalBounds.top,
        metrics96.leftPadding + metrics96.iconColumnWidth,
        normalBounds.bottom,
    };
    Expect(CountBlueAccentPixelsInRect(pixels, kWidth, kHeight,
            packageImageColumn) > 0,
        "package image pixels remain visible in the menu icon column");
    DeleteObject(packageImage);

    const snowdesktop::menu_icon::ItemView inlinePrevious{
        L"", L"\uF15B", false, false, false,
    };
    const snowdesktop::menu_icon::ItemView inlineStatus{
        L"1 / 3", L"", false, false, false,
    };
    const RECT inlinePreviousBounds{ 0, 0, 40, metrics96.rowHeight };
    const RECT inlineStatusBounds{ 40, 0, 200, metrics96.rowHeight };
    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawInlineAction(
        dc, font, fluentFont, inlinePrevious, inlinePreviousBounds,
        ODS_SELECTED, light, metrics96),
        "selected inline paging arrow renders");
    Expect(snowdesktop::menu_icon::DrawInlineAction(
        dc, font, fluentFont, inlineStatus, inlineStatusBounds,
        ODS_DISABLED | ODS_GRAYED, light, metrics96),
        "disabled inline page status renders");
    Expect(CountColorInRect(pixels, kWidth, kHeight,
        inlinePreviousBounds, light.hoverBackground) > 0,
        "inline paging button keeps the menu hover treatment");
    Expect(CountColorInRect(pixels, kWidth, kHeight,
        inlineStatusBounds, light.disabledText) > 0,
        "inline page status uses disabled menu text");

    const snowdesktop::menu_icon::ItemView checkedFilter{
        L"Installed 3", L"", false, false, true,
    };
    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawInlineAction(
        dc, font, fluentFont, checkedFilter, inlineStatusBounds,
        0, light, metrics96),
        "checked source filter renders");
    Expect(CountColorInRect(pixels, kWidth, kHeight,
        inlineStatusBounds, light.hoverBackground) > 0 &&
        CountBlueAccentPixelsInRect(pixels, kWidth, kHeight,
            inlineStatusBounds) > 0,
        "checked source filter keeps a persistent accented tag state");

    const snowdesktop::menu_icon::ItemView searchInput{
        L"Search components", L"\uF68F", false, false, false,
    };
    std::fill_n(pixels, kWidth * kHeight, 0u);
    const snowdesktop::menu_icon::TextInputView activeSearch{
        L"clock", 3, 3, L"shi", 2, true, true,
    };
    Expect(snowdesktop::menu_icon::DrawTextInput(
        dc, font, fluentFont, searchInput, activeSearch,
        normalBounds, light, metrics96),
        "active component search input renders");

    std::fill_n(pixels, kWidth * kHeight, 0u);
    const snowdesktop::menu_icon::TextInputView emptySearch{
        L"", 0, 0, L"", 0, true, true,
    };
    Expect(snowdesktop::menu_icon::DrawTextInput(
        dc, font, fluentFont, searchInput, emptySearch,
        normalBounds, light, metrics96),
        "focused empty component search input renders");
    // The middle of the field excludes its rounded accent border. With an
    // empty value and a gray placeholder, the caret is the only blue content.
    const RECT fieldInterior{
        normalBounds.left + metrics96.outerInset + metrics96.selectionRadius,
        normalBounds.top + metrics96.rowHeight / 3,
        normalBounds.right - metrics96.outerInset - metrics96.selectionRadius,
        normalBounds.bottom - metrics96.rowHeight / 3,
    };
    Expect(CountBlueAccentPixelsInRect(pixels, kWidth, kHeight,
            fieldInterior) > 0,
        "focused empty search input paints its caret inside the field");
    auto hiddenCaretSearch = emptySearch;
    hiddenCaretSearch.caretVisible = false;
    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawTextInput(
            dc, font, fluentFont, searchInput, hiddenCaretSearch,
            normalBounds, light, metrics96),
        "the same focused search field renders with the caret hidden");
    Expect(CountBlueAccentPixelsInRect(pixels, kWidth, kHeight,
            fieldInterior) == 0 &&
            CountBlueAccentPixelsInRect(pixels, kWidth, kHeight, normalBounds) > 0,
        "hiding the caret removes interior accent pixels while retaining the border");

    const std::array accentedQuickIcons{
        snowdesktop::MenuQuickIcon::NewItem,
        snowdesktop::MenuQuickIcon::Cut,
        snowdesktop::MenuQuickIcon::Copy,
        snowdesktop::MenuQuickIcon::Rename,
    };
    for (const auto quickIcon : accentedQuickIcons)
    {
        std::fill_n(pixels, kWidth * kHeight, 0u);
        Expect(snowdesktop::menu_icon::DrawQuickAction(
            dc, font, fluentFont, quickIcon, normal, quickBounds,
            0, light, metrics96),
            "semantic two-tone quick action renders");
        Expect(CountBlueAccentPixels(pixels, kWidth * kHeight) > 0,
            "semantic two-tone quick action retains its accent component");
    }

    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawQuickAction(
        dc, font, fluentFont, snowdesktop::MenuQuickIcon::NewItem,
        normal, quickBounds, 0, dark, metrics96),
        "dark add-circle quick action renders");
    const RECT newIconBounds{
        kWidth / 2 - 16,
        quickBounds.top + metrics96.outerInset,
        kWidth / 2 + 16,
        quickBounds.top + metrics96.outerInset +
            metrics96.quickActionIconHeight,
    };
    Expect(CountBlueAccentPixelsInRect(pixels, kWidth, kHeight,
        newIconBounds) > 0,
        "dark add-circle keeps a visible blue plus");
    Expect(CountColorInRect(pixels, kWidth, kHeight,
        newIconBounds, dark.text) > 0,
        "dark add-circle keeps a neutral Fluent ring");

    const snowdesktop::menu_icon::ItemView moreOptionsItem{
        L"Show more options", L"\uF582", false, false, false,
        snowdesktop::MenuQuickIcon::Open,
    };
    const RECT moreOptionsBounds{ 0, 0, kWidth, metrics96.rowHeight };
    const RECT moreOptionsIconBounds{
        metrics96.leftPadding,
        0,
        metrics96.leftPadding + metrics96.iconColumnWidth,
        metrics96.rowHeight,
    };
    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawItem(
        dc, font, fluentFont, moreOptionsItem, moreOptionsBounds,
        0, light, metrics96),
        "Explorer-style more-options row icon renders");
    Expect(CountBlueAccentPixelsInRect(pixels, kWidth, kHeight,
        moreOptionsIconBounds) > 0,
        "more-options row colors only its open arrow with the accent");
    Expect(CountColorInRect(pixels, kWidth, kHeight,
        moreOptionsIconBounds, light.text) > 0,
        "more-options row keeps a neutral window outline");

    std::fill_n(pixels, kWidth * kHeight, 0u);
    Expect(snowdesktop::menu_icon::DrawItem(
        dc, font, fluentFont, moreOptionsItem, moreOptionsBounds,
        ODS_DISABLED | ODS_GRAYED, light, metrics96),
        "disabled more-options row icon renders");
    Expect(CountBlueAccentPixelsInRect(pixels, kWidth, kHeight,
        moreOptionsIconBounds) == 0,
        "disabled more-options row does not retain a blue accent");

    const std::array<const wchar_t*, 11> alignedMenuGlyphs{
        snowdesktop::menu_fluent_glyphs::kFanExpansion,
        L"\uF33A", L"\uF32B", L"\uF10C", L"\U000F0A39",
        L"\uF3DD", L"\uF34C", L"\uF6A9", L"\uF21D",
        L"\uF15B", L"\uF181",
    };
    const std::array<UINT, 4> iconTestDpis{ 96, 120, 144, 192 };
    for (const UINT dpi : iconTestDpis)
    {
        const auto dpiMetrics =
            snowdesktop::menu_icon::ResolveMetrics(dpi);
        HFONT dpiFluentFont = CreateFontW(
            -dpiMetrics.quickActionFontHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"FluentSystemIcons-Regular");
        Expect(dpiFluentFont != nullptr,
            "DPI-specific Fluent Regular font is created");

        std::fill_n(pixels, kWidth * kHeight, 0u);
        const RECT dpiQuickBounds{
            0, 0, kWidth, dpiMetrics.quickActionHeight,
        };
        Expect(snowdesktop::menu_icon::DrawQuickAction(
            dc, font, dpiFluentFont,
            snowdesktop::MenuQuickIcon::NewItem, normal,
            dpiQuickBounds, 0, dark, dpiMetrics),
            "Fluent quick icon renders at a common monitor DPI");
        Expect(CountBlueAccentPixels(pixels, kWidth * kHeight) > 0,
            "Fluent quick icon retains its accent across common DPIs");

        std::fill_n(pixels, kWidth * kHeight, 0u);
        const snowdesktop::menu_icon::ItemView settingsItem{
            L"Settings", L"\uF6A9", false, false, false,
        };
        const RECT dpiRowBounds{ 0, 0, kWidth, dpiMetrics.rowHeight };
        Expect(snowdesktop::menu_icon::DrawItem(
            dc, font, dpiFluentFont, settingsItem, dpiRowBounds,
            0, dark, dpiMetrics),
            "Fluent row icon renders at a common monitor DPI");
        const RECT dpiIconColumn{
            dpiMetrics.leftPadding,
            0,
            dpiMetrics.leftPadding + dpiMetrics.iconColumnWidth,
            dpiMetrics.rowHeight,
        };
        const int iconInk = CountPixelsDifferentFromColorInRect(
            pixels, kWidth, kHeight, dpiIconColumn, dark.background);
        const int iconArea =
            (dpiIconColumn.right - dpiIconColumn.left) *
            (dpiIconColumn.bottom - dpiIconColumn.top);
        Expect(iconInk > 0 && iconInk < iconArea / 2,
            "Fluent row icon keeps its outline instead of becoming a block");
        for (const wchar_t* glyph : alignedMenuGlyphs)
        {
            const snowdesktop::menu_icon::ItemView alignedIconItem{
                L"Settings", glyph, false, false, false,
            };
            std::fill_n(pixels, kWidth * kHeight, 0u);
            Expect(snowdesktop::menu_icon::DrawItem(
                dc, font, dpiFluentFont, alignedIconItem,
                dpiRowBounds, 0, dark, dpiMetrics),
                "common Fluent menu icon renders for alignment checks");
            const RECT iconInkBounds =
                FindPixelsDifferentFromColorInRect(
                    pixels, kWidth, kHeight,
                    dpiIconColumn, dark.background);
            Expect(iconInkBounds.right > iconInkBounds.left &&
                    iconInkBounds.bottom > iconInkBounds.top,
                "common Fluent menu icon produces visible ink");
            const int rowCenterTwice =
                dpiRowBounds.top + dpiRowBounds.bottom - 1;
            const int inkCenterTwice =
                iconInkBounds.top + iconInkBounds.bottom - 1;
            Expect(std::abs(rowCenterTwice - inkCenterTwice) <= 1,
                "common Fluent menu icons stay vertically centered across DPI");
        }
        DeleteObject(dpiFluentFont);
    }

    const std::array neutralQuickIcons{
        snowdesktop::MenuQuickIcon::Paste,
        snowdesktop::MenuQuickIcon::Refresh,
        snowdesktop::MenuQuickIcon::Delete,
        snowdesktop::MenuQuickIcon::Edit,
        snowdesktop::MenuQuickIcon::Settings,
    };
    for (const auto quickIcon : neutralQuickIcons)
    {
        std::fill_n(pixels, kWidth * kHeight, 0u);
        Expect(snowdesktop::menu_icon::DrawQuickAction(
            dc, font, fluentFont, quickIcon, normal, quickBounds,
            0, light, metrics96),
            "monochrome quick action renders");
        Expect(CountBlueAccentPixels(pixels, kWidth * kHeight) == 0,
            "monochrome quick action does not receive an arbitrary accent");
    }

    if (argc == 3 && wcscmp(argv[1], L"--snapshot") == 0)
    {
        Expect(SavePreviewBitmap(argv[2], bitmapInfo.bmiHeader,
            rawPixels, bitmapInfo.bmiHeader.biSizeImage),
            "menu preview bitmap is written");
    }

    DeleteObject(submenuArrowFont);
    DeleteObject(fluentFont);
    DeleteObject(font);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    RemoveFontMemResourceEx(fluentFontHandle);
    std::cout << "menu owner-draw render tests passed\n";
    return 0;
}
