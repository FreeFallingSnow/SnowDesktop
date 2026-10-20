#pragma once

#include "widget_preview_stage.h"

#include <wincodec.h>

#include <limits>
#include <new>
#include <stdexcept>

namespace snowdesktop::widget_preview
{
// Keep source dimensions for Windows placement/crop semantics. Images beyond
// this decoded budget use the caller's existing neutral preview fallback.
inline constexpr std::size_t MaximumDecodedWallpaperBytes = 256u * 1024u * 1024u;

inline Wallpaper DecodeWallpaperPixels(IWICBitmapSource* source)
{
    if (!source) return {};
    UINT width = 0, height = 0;
    if (FAILED(source->GetSize(&width, &height)) || !width || !height ||
        width > static_cast<UINT>((std::numeric_limits<int>::max)()) ||
        height > static_cast<UINT>((std::numeric_limits<int>::max)()))
        return {};

    constexpr std::size_t MaximumPixels = MaximumDecodedWallpaperBytes / sizeof(std::uint32_t);
    // Divide before multiplying so this boundary is also safe in a 32-bit host.
    if (width > MaximumPixels || height > MaximumPixels / width)
        return {};
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    const std::size_t byteCount = pixelCount * sizeof(std::uint32_t);
    static_assert(MaximumDecodedWallpaperBytes <= (std::numeric_limits<UINT>::max)());
    Wallpaper result;
    try { result.pixels.resize(pixelCount); }
    catch (const std::bad_alloc&) { return {}; }
    catch (const std::length_error&) { return {}; }
    const UINT stride = width * sizeof(std::uint32_t);
    if (FAILED(source->CopyPixels(nullptr, stride, static_cast<UINT>(byteCount),
            reinterpret_cast<BYTE*>(result.pixels.data()))))
        return {};
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    // WIC supplies straight BGRA. Preview PNGs remain opaque over the same
    // blue-gray compatibility backdrop used by the preview stage.
    for (std::uint32_t& pixel : result.pixels)
    {
        const unsigned alpha = pixel >> 24;
        const auto composite = [alpha](unsigned foreground, unsigned background) {
            return (foreground * alpha + background * (255u - alpha) + 127u) / 255u;
        };
        const unsigned blue = composite(pixel & 0xffu, 45);
        const unsigned green = composite((pixel >> 8) & 0xffu, 39);
        const unsigned red = composite((pixel >> 16) & 0xffu, 32);
        pixel = 0xff000000u | blue | (green << 8) | (red << 16);
    }
    return result;
}
}
