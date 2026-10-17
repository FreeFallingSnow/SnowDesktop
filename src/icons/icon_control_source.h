#pragma once
#include "icon_image_codec.h"
#include "icon_beautify.h"
#include "icon_render_rules.h"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace snowdesktop::icon_control
{
// Classify a replacement at a stable resolution; resizing before detection can
// change a plate into a padded glyph as the pointer enters or leaves the Dock.
inline int ReplacementSourcePixels(std::wstring_view identity, int targetPixels)
{
    return identity == L"control:start" || identity == L"control:search"
        ? icon_render_rules::kMaximumSourcePixels : icon_render_rules::SourcePixelsForTarget(targetPixels);
}
// Rendering is independent of hover: interactive feedback belongs to Dock.
inline bool UsesBitmap(std::wstring_view identity, bool independent, const IconBeautifySettings& style)
{
    return (identity == L"control:start" || identity == L"control:search") && (independent || style.enabled);
}
inline icon_image::Image MakeSource(std::wstring_view target, bool light, int edge = 128)
{
    // Supersampled native glyphs, without an already-beautified Dock plate.
    edge = std::clamp(edge, 128, 256);
    const auto extent = static_cast<float>(edge);
    snowdesktop::icon_image::Image image; image.width = image.height = edge;
    image.pixels.resize(static_cast<size_t>(edge) * edge);
    const bool start = target == L"control:start";
    for (int y = 0; y < edge; ++y) for (int x = 0; x < edge; ++x)
    {
        int coverage = 0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx)
        {
            const float px = (static_cast<float>(x) + (static_cast<float>(sx) + .5f) / 4.f) / extent, py = (static_cast<float>(y) + (static_cast<float>(sy) + .5f) / 4.f) / extent;
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
        image.pixels[static_cast<size_t>(y) * edge + x] = (static_cast<std::uint32_t>(alpha) << 24) | static_cast<std::uint32_t>(rgb);
    }
    return image;
}

}
