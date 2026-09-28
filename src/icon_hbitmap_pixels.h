#pragma once

#include <windows.h>
#include "icon_bitmap_pixels.h"

namespace snowdesktop::icon_bitmap_pixels
{
struct Buffer
{
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
};

// Host-private input for icon analysis, beautification and D2D uploads. Neither
// BITMAP nor the header returned by GetObject reliably retains a DIB's original
// row direction. Ask GDI for top-down pixels, including CopyImage cache clones.
inline bool ReadHBitmap(HBITMAP bitmap, Buffer& out)
{
    out = {};
    BITMAP bm{};
    if (!bitmap || !GetObjectW(bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;

    const int width = bm.bmWidth;
    const int height = bm.bmHeight;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height);
    HDC dc = GetDC(nullptr);
    if (!dc) { out = {}; return false; }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int rows = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height),
        out.pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (rows != height) { out = {}; return false; }
    NormalizeShellPixels(out.pixels);
    return true;
}
}
