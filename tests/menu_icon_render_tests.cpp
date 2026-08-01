#include "menu_icon_render.h"

#include <algorithm>
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
    return static_cast<std::uint32_t>(GetRValue(color)) |
        (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
        (static_cast<std::uint32_t>(GetBValue(color)) << 16);
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
    const auto light = snowdesktop::menu_icon::ResolvePalette(true);
    const auto dark = snowdesktop::menu_icon::ResolvePalette(false);
    Expect(light.background != light.disabledText,
        "light disabled text contrasts with the menu background");
    Expect(dark.background != dark.disabledText,
        "dark disabled text contrasts with the menu background");

    const auto metrics96 = snowdesktop::menu_icon::ResolveMetrics(96);
    const auto metrics192 = snowdesktop::menu_icon::ResolveMetrics(192);
    Expect(metrics96.rowHeight == 34,
        "menu uses the intended compact Win11 row height");
    Expect(metrics192.rowHeight == metrics96.rowHeight * 2,
        "menu row height follows monitor DPI");

    constexpr int kWidth = 240;
    constexpr int kHeight = 120;
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
    const RECT disabledBounds{ 0, 34, kWidth, 68 };
    const RECT selectedBounds{ 0, 68, kWidth, 102 };
    const RECT separatorBounds{ 0, 102, kWidth, 111 };
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
    Expect(disabledColorPixels > 0,
        "disabled text and icon remain visibly gray without hover");
    Expect(disabledColorPixels < metrics96.rowHeight *
        metrics96.iconColumnWidth,
        "disabled glyph retains its shape instead of becoming a block");
    Expect(hoverColorPixels > 0,
        "selected row has a rounded Win11-style hover fill");
    Expect(separatorColorPixels > 0,
        "separator uses the themed separator color");

    if (argc == 3 && wcscmp(argv[1], L"--snapshot") == 0)
    {
        Expect(SavePreviewBitmap(argv[2], bitmapInfo.bmiHeader,
            rawPixels, bitmapInfo.bmiHeader.biSizeImage),
            "menu preview bitmap is written");
    }

    DeleteObject(font);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    std::cout << "menu owner-draw render tests passed\n";
    return 0;
}
