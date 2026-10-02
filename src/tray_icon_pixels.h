#pragma once
#include "taskbar_hook/tray_protocol.h"

namespace snowdesktop::tray
{
inline void Pixels(HICON icon, Event& event)
{
    if (!icon) return;
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return;
    BITMAP bitmap{};
    const bool color = info.hbmColor != nullptr;
    const bool valid = GetObjectW(color ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap) != 0;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (!valid || bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0) return;
    const int width = (std::min)(bitmap.bmWidth, static_cast<LONG>(kIconSize));
    const int height = (std::min)(bitmap.bmHeight / (color ? 1 : 2), static_cast<LONG>(kIconSize));
    if (height <= 0) return;
    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = width; dib.bmiHeader.biHeight = -height;
    dib.bmiHeader.biPlanes = 1; dib.bmiHeader.biBitCount = 32; dib.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP image = CreateDIBSection(dc, &dib, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dc && image && bits)
    {
        HGDIOBJ old = SelectObject(dc, image);
        const auto count = static_cast<std::size_t>(width) * height;
        std::memset(bits, 0, count * 4);
        if (DrawIconEx(dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL))
        {
            GdiFlush();
            std::copy_n(static_cast<std::uint32_t*>(bits), count, event.pixels.begin());
            const bool alpha = std::any_of(event.pixels.begin(), event.pixels.begin() + count,
                [](auto pixel) { return (pixel >> 24) != 0; });
            if (!alpha)
            {
                // Mask icons (including opaque black pixels) need the AND mask;
                // derive it by rendering against white as well as black.
                std::fill_n(static_cast<std::uint32_t*>(bits), count, 0x00ffffffu);
                DrawIconEx(dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL);
                GdiFlush();
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto black = event.pixels[i] & 255, white = static_cast<std::uint32_t*>(bits)[i] & 255;
                    const auto a = 255 - (white >= black ? white - black : 0);
                    event.pixels[i] = (event.pixels[i] & 0xffffff) | (a << 24);
                }
            }
            event.width = static_cast<DWORD>(width); event.height = static_cast<DWORD>(height);
        }
        SelectObject(dc, old);
    }
    if (image) DeleteObject(image);
    if (dc) DeleteDC(dc);
}

}
