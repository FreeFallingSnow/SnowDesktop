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
        check(framedFill && framedFill->cover && framedFill->inset > 0 &&
            framedResult[static_cast<size_t>(margin) * size + size / 2] == rgb(245, 245, 245),
            "thin straight rim is cropped and its solid interior covers the theme without shrinking");
        const auto disc = plate(3);
        const auto discFill = beauty::DetectPlateFill(disc, size, size);
        const auto discResult = beauty::Render(disc, size, size, style, discFill);
        check(discFill && discFill->gradient &&
            std::abs(static_cast<int>((discResult[static_cast<size_t>(size / 2) * size + margin / 2] >> 8) & 255) - 245) <= 2,
            "shaded circular plate extends its spatial gradient without an averaged ghost circle");
        auto discColor = discFill;
        if (!discColor) discColor = beauty::DetectEdgeFill(disc, size, size);
        check(discColor.has_value(),
            "a protected disc can still supply the large-icon color picker without authorizing smart clipping");

        for (const auto color : {rgb(252, 30, 30), rgb(28, 144, 237), rgb(248, 248, 248)})
        {
            auto circle = disc;
            for (auto& pixel : circle) if (pixel) pixel = color;
            circle[static_cast<size_t>(size / 2) * size + size / 2] = rgb(255, 255, 255);
            const auto fill = beauty::DetectPlateFill(circle, size, size);
            check(fill && !fill->gradient && beauty::Render(circle, size, size, style, fill)
                [static_cast<size_t>(size / 2) * size + margin / 2] == color,
                "continuous red blue and neutral circular plates are eligible rather than categorically rejected");
        }

        auto photograph = solid;
        for (int cy = margin; cy < size - margin; ++cy)
            for (int cx = margin; cx < size - margin; ++cx)
                photograph[static_cast<size_t>(cy) * size + cx] =
                    ((cx * 8 / size + cy * 8 / size) % 2) ? rgb(180, 65, 40) : rgb(35, 80, 160);
        const auto photoFill = beauty::DetectPlateFill(photograph, size, size);
        const auto photoResult = beauty::Render(photograph, size, size, style, photoFill);
        check(photoFill && photoFill->cover &&
            (photoResult[static_cast<size_t>(margin) * size + size / 2] >> 24) == 255,
            "complete photographic tiles crop real image pixels to the theme without requiring a uniform fill or shrinking");

        // Some pixel-art tiles encode their eyes as transparent cutouts. Keep
        // those holes transparent over the theme, while fitting the full tile.
        auto cutouts = std::vector<std::uint32_t>(static_cast<size_t>(size) * size, rgb(40, 140, 230));
        for (int cy = size * 7 / 16; cy < size * 9 / 16; ++cy)
            for (int cx = size * 5 / 16; cx < size * 7 / 16; ++cx) cutouts[static_cast<size_t>(cy) * size + cx] = 0;
        for (int cy = size * 7 / 16; cy < size * 9 / 16; ++cy)
            for (int cx = size * 10 / 16; cx < size * 12 / 16; ++cx) cutouts[static_cast<size_t>(cy) * size + cx] = 0;
        const auto cutoutFill = beauty::DetectPlateFill(cutouts, size, size);
        const auto cutoutResult = beauty::Render(cutouts, size, size, style, cutoutFill);
        check(cutoutFill && cutoutFill->cover &&
            cutoutResult[static_cast<size_t>(size / 2) * size + size * 6 / 16] == 0 &&
            (cutoutResult[static_cast<size_t>(size / 2) * size + size / 12] >> 24) == 255,
            "small transparent details in a complete image tile keep the theme visible without shrinking");

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
