#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace snowdesktop::icon_bitmap_pixels
{
// Shell handlers can return either straight or premultiplied BGRA. Normalize
// once before passing their DIB to WIC/D2D, which expect premultiplied samples.
inline void NormalizeShellPixels(std::vector<std::uint32_t>& pixels)
{
    const bool hasAlpha = std::any_of(pixels.begin(), pixels.end(), [](auto p) { return p >> 24; });
    if (!hasAlpha)
        for (auto& p : pixels) if (p & 0xffffff) p |= 0xff000000;
    const bool straight = std::any_of(pixels.begin(), pixels.end(), [](auto p) {
        const auto a = p >> 24;
        return a > 0 && a < 255 && (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a);
    });
    for (auto& p : pixels)
    {
        const auto a = p >> 24;
        if (!a) { p = 0; continue; }
        if (!straight || a == 255) continue;
        const auto channel = [&](int shift) { return ((((p >> shift) & 255) * a + 127) / 255) << shift; };
        p = (a << 24) | channel(16) | channel(8) | channel(0);
    }
}
}
