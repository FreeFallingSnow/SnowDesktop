#include "menu_icon_render.h"
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
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL,
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
        selectedBounds, ODS_SELECTED, light, metrics96),
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

    DeleteObject(fluentFont);
    DeleteObject(font);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    RemoveFontMemResourceEx(fluentFontHandle);
    std::cout << "menu owner-draw render tests passed\n";
    return 0;
}
