#pragma once

#include "icons/icon_beautify.h"
#include <algorithm>
#include <cmath>
#include <iostream>

// Draw equivalent geometry rather than redistributing installed application art.
// These assertions run the production detector and compositor together.
inline int RunIconBackplateTests()
{
    namespace beauty = snowdesktop::icon_beautify;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    const auto rgb = [](int r, int g, int b) {
        return 0xff000000u | static_cast<unsigned>(r << 16 | g << 8 | b);
    };
    auto style = beauty::MakePreset(snowdesktop::IconBeautifyPreset::FrostedGlassLight);
    style.backgroundOpacity = 0; style.shadowStrength = 0;
    style.edgeHighlightEnabled = false; style.outlineEnabled = false;
    style.textureHighlightStrength = style.textureShadeStrength = style.textureEdgeHighlight = 0;
    style.contentScale = .68f;
    for (int size : {64, 96, 128, 192, 256})
    {
        const int margin = size / 16;
        const auto plate = [&](int kind) {
            std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * size, 0);
            for (int y = margin; y < size - margin; ++y)
                for (int x = margin; x < size - margin; ++x)
                {
                    const double dx = x + .5 - size * .5, dy = y + .5 - size * .5;
                    if (kind == 3 && dx * dx + dy * dy > (size * .4375) * (size * .4375)) continue;
                    const int green = 80 + static_cast<int>(120. * (y + .5) / size);
                    std::uint32_t color = kind == 1 ? rgb(20, green, 110) : rgb(30, 140, 110);
                    if (kind == 2) color = x < margin + std::max(1, size / 64) || y < margin + std::max(1, size / 64) ||
                        x >= size - margin - std::max(1, size / 64) || y >= size - margin - std::max(1, size / 64)
                        ? rgb(40, 40, 40) : rgb(245, 245, 245);
                    if (kind == 3) { const int white = 255 - y * 20 / size; color = rgb(white, white, white); }
                    if (x > size * .42 && x < size * .58 && y > size * .35 && y < size * .65) color = rgb(210, 40, 60);
                    pixels[static_cast<size_t>(y) * size + x] = color;
                }
            return pixels;
        };
        const auto solid = plate(0);
        const auto solidFill = beauty::DetectPlateFill(solid, size, size);
        const auto solidResult = beauty::Render(solid, size, size, style, solidFill);
        check(solidFill && solidFill->r == 30 && solidFill->g == 140 &&
            solidResult[static_cast<size_t>(size / 2) * size + margin / 2] == rgb(30, 140, 110),
            "uniform complete plate extends its original color through transparent margins at every source bucket");

        const auto gradient = plate(1);
        const auto gradientFill = beauty::DetectPlateFill(gradient, size, size);
        const auto gradientResult = beauty::Render(gradient, size, size, style, gradientFill);
        const int x = size / 2, y = margin / 2;
        const auto extended = gradientResult[static_cast<size_t>(y) * size + x];
        check(gradientFill && (extended >> 24) == 255 &&
            std::abs(static_cast<int>((extended >> 8) & 255) - (80 + static_cast<int>(120. * (y + .5) / size))) <= 2,
            "continuous gradient is extrapolated spatially instead of flattened to an average edge color");
        check(gradientResult[static_cast<size_t>(size / 2) * size + size / 2] == rgb(210, 40, 60),
            "gradient extension retains original opaque logo pixels");

        const auto framed = plate(2);
        const auto framedFill = beauty::DetectPlateFill(framed, size, size);
        const auto framedResult = beauty::Render(framed, size, size, style, framedFill);
        check(!framedFill && framedResult[static_cast<size_t>(margin) * size + size / 2] == 0,
            "straight decorative border moves intact into foreground fit rather than being clipped into the theme outline");
        const auto disc = plate(3);
        const auto discFill = beauty::DetectPlateFill(disc, size, size);
        check(!discFill && beauty::Render(disc, size, size, style, discFill)[static_cast<size_t>(margin) * size + size / 2] == 0,
            "shaded circular disc keeps its contour instead of becoming a ghost circle in an invented square plate");

        auto hollow = solid;
        for (int cy = size * 3 / 8; cy < size * 5 / 8; ++cy)
            for (int cx = size * 3 / 8; cx < size * 5 / 8; ++cx) hollow[static_cast<size_t>(cy) * size + cx] = 0;
        check(!beauty::DetectPlateFill(hollow, size, size), "transparent interior holes cannot become a solid backplate");

        // Folded page with an overlapping colored application badge. It is dense
        // and mostly white, but its asymmetric silhouette is meaningful content.
        std::vector<std::uint32_t> document(static_cast<size_t>(size) * size, 0);
        for (int cy = size / 16; cy < size * 15 / 16; ++cy)
            for (int cx = size * 3 / 16; cx < size * 14 / 16; ++cx)
                if (cx < size * 11 / 16 || cy >= size * 4 / 16 || cx + cy < size * 15 / 16)
                    document[static_cast<size_t>(cy) * size + cx] = rgb(246, 246, 246);
        for (int cy = size * 6 / 16; cy < size * 13 / 16; ++cy)
            for (int cx = size / 16; cx < size * 8 / 16; ++cx) document[static_cast<size_t>(cy) * size + cx] = rgb(20, 70, 160);
        check(!beauty::DetectPlateFill(document, size, size),
            "folded document and overlapping badge remain foreground even when the white body dominates edge samples");
    }
    check(!beauty::DetectPlateFill({}, 128, 128) && !beauty::DetectPlateFill(std::vector<std::uint32_t>(64 * 64), 64, 64),
        "invalid and empty images never supply an automatic fill");
    return failures;
}
