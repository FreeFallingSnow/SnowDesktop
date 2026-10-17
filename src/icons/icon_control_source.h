#pragma once
#include "icon_image_codec.h"
#include "icon_beautify.h"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace snowdesktop::icon_control
{
// Rendering is independent of hover: interactive feedback belongs to Dock.
inline bool UsesBitmap(std::wstring_view identity, bool independent, const IconBeautifySettings& style)
{
    return (identity == L"control:start" || identity == L"control:search") && (independent || style.enabled);
}
inline icon_image::Image MakeSource(std::wstring_view target, bool light)
{
    // Supersampled native glyphs, without an already-beautified Dock plate.
    snowdesktop::icon_image::Image image; image.width = image.height = 128;
    image.pixels.resize(128 * 128);
    const bool start = target == L"control:start";
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x)
    {
        int coverage = 0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx)
        {
            const float px = (static_cast<float>(x) + (static_cast<float>(sx) + .5f) / 4.f) / 128.f, py = (static_cast<float>(y) + (static_cast<float>(sy) + .5f) / 4.f) / 128.f;
            bool inside;
            if (start) inside = px >= .21f && px <= .79f && py >= .21f && py <= .79f &&
                !(px > .474f && px < .526f) && !(py > .474f && py < .526f);
            else
            {
                const float dx = px - .448f, dy = py - .448f;
                const float ring = std::abs(std::sqrt(dx * dx + dy * dy) - .177f);
                const float line = std::clamp((dx + dy - .254f) / .265f, 0.f, 1.f);
                const float lx = dx - (.127f + .1325f * line), ly = dy - (.127f + .1325f * line);
                inside = ring <= .0226f || lx * lx + ly * ly <= .0226f * .0226f;
            }
            if (inside) ++coverage;
        }
        const auto alpha = coverage * (start ? 240 : light ? 184 : 235) / 16;
        const auto rgb = start ? ((120 * alpha / 255) << 8) | (214 * alpha / 255) :
            light ? 0 : (alpha << 16) | (alpha << 8) | alpha;
        image.pixels[static_cast<size_t>(y) * 128 + x] = (static_cast<std::uint32_t>(alpha) << 24) | static_cast<std::uint32_t>(rgb);
    }
    return image;
}

}
