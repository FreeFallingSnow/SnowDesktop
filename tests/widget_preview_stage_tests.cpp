#include "widget_preview_stage.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}
}

int main()
{
    using namespace snowdesktop::widget_preview;
    const Wallpaper dark = GenerateWallpaper(96, 72, false);
    const Wallpaper repeated = GenerateWallpaper(96, 72, false);
    const Wallpaper light = GenerateWallpaper(96, 72, true);
    Check(dark.width == 96 && dark.height == 72 &&
            dark.pixels.size() == 96u * 72u,
        "preview wallpaper preserves its requested dimensions");
    Check(dark.pixels == repeated.pixels,
        "preview wallpaper generation is deterministic");
    Check(dark.pixels == light.pixels,
        "the compatibility background is appearance-independent");
    Check(std::all_of(dark.pixels.begin(), dark.pixels.end(),
            [](std::uint32_t pixel) { return (pixel >> 24) == 0xffu; }) &&
          std::all_of(light.pixels.begin(), light.pixels.end(),
            [](std::uint32_t pixel) { return (pixel >> 24) == 0xffu; }),
        "preview wallpaper is fully opaque");
    const std::unordered_set<std::uint32_t> darkColors(
        dark.pixels.begin(), dark.pixels.end());
    const std::unordered_set<std::uint32_t> lightColors(
        light.pixels.begin(), light.pixels.end());
    Check(darkColors.size() > 512 && lightColors.size() > 512,
        "neutral preview fallback retains enough visual detail for blur");
    Check(GenerateWallpaper(1, 1, false).pixels.size() == 1 &&
            GenerateWallpaper(0, 72, false).pixels.empty() &&
            GenerateWallpaper(96, -1, true).pixels.empty(),
        "preview wallpaper handles minimum and invalid dimensions");

    const Wallpaper full = GenerateWallpaper(240, 180, false);
    const Wallpaper crop = GenerateWallpaper(80, 60, false,
        { 240, 180, 47, 33 });
    bool cropMatches = crop.width == 80 && crop.height == 60;
    for (int y = 0; y < crop.height && cropMatches; ++y)
    {
        for (int x = 0; x < crop.width; ++x)
        {
            if (crop.pixels[static_cast<std::size_t>(y) * crop.width + x] !=
                full.pixels[static_cast<std::size_t>(y + 33) * full.width +
                    x + 47])
            {
                cropMatches = false;
                break;
            }
        }
    }
    Check(cropMatches,
        "preview wallpaper crops preserve one continuous composition");

    Wallpaper selectedSource;
    selectedSource.width = 240;
    selectedSource.height = 180;
    selectedSource.pixels.resize(240u * 180u);
    for (int y = 0; y < selectedSource.height; ++y)
    {
        for (int x = 0; x < selectedSource.width; ++x)
        {
            selectedSource.pixels[static_cast<std::size_t>(y) *
                selectedSource.width + x] = 0xff000000u |
                static_cast<std::uint32_t>(x) |
                (static_cast<std::uint32_t>(y) << 8) |
                (static_cast<std::uint32_t>((x + y) & 0xff) << 16);
        }
    }
    const Wallpaper selectedFull = GenerateWallpaper(
        selectedSource, 240, 180);
    const Wallpaper selectedCrop = GenerateWallpaper(
        selectedSource, 80, 60, { 240, 180, 47, 33 });
    Check(selectedCrop.pixels.front() ==
            selectedFull.pixels[33u * 240u + 47u] &&
            selectedCrop.pixels.back() ==
            selectedFull.pixels[92u * 240u + 126u],
        "an explicit wallpaper preserves the shared card crop");
    Check(WallpaperFingerprint(selectedSource) ==
            WallpaperFingerprint(selectedSource) &&
            WallpaperFingerprint(selectedSource) !=
                WallpaperFingerprint(dark),
        "wallpaper fingerprints are stable and source-sensitive");
    Wallpaper white{ 1, 1, { 0xffffffffu } };
    Wallpaper black{ 1, 1, { 0xff000000u } };
    Check(WallpaperIsLight(white) && !WallpaperIsLight(black),
        "wallpaper luminance selects contrasting card chrome");

    Wallpaper monitor;
    monitor.width = 4;
    monitor.height = 3;
    monitor.pixels = {
        0xff000001u, 0xff000002u, 0xff000003u, 0xff000004u,
        0xff000005u, 0xff000006u, 0xff000007u, 0xff000008u,
        0xff000009u, 0xff00000au, 0xff00000bu, 0xff00000cu,
    };
    const Wallpaper positionedCrop = CropWallpaper(monitor,
        { -1920, 0, -1916, 3 }, { -1918, 1, -1916, 3 });
    Check(positionedCrop.width == 2 && positionedCrop.height == 2 &&
            positionedCrop.pixels == std::vector<std::uint32_t>{
                0xff000007u, 0xff000008u,
                0xff00000bu, 0xff00000cu },
        "screen capture crop preserves physical position without scaling");
    Check(CropWallpaper(monitor, { 0, 0, 4, 3 },
            { 3, 2, 5, 3 }).pixels.empty(),
        "screen capture crop rejects rectangles outside the monitor frame");

    const AcrylicNoisePixels darkNoise = GenerateAcrylicNoise(false);
    const AcrylicNoisePixels repeatedNoise = GenerateAcrylicNoise(false);
    const AcrylicNoisePixels lightNoise = GenerateAcrylicNoise(true);
    Check(darkNoise == repeatedNoise,
        "acrylic noise generation is deterministic");
    Check(darkNoise != lightNoise,
        "acrylic noise polarity follows the content theme");
    for (std::size_t index = 0; index < darkNoise.size(); ++index)
    {
        Check((darkNoise[index] >> 24) == (lightNoise[index] >> 24),
            "acrylic theme variants preserve the same alpha texture");
    }
    std::cout << "widget preview stage tests passed\n";
    return 0;
}
