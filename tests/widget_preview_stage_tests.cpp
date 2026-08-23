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
        "all component appearances share one landscape wallpaper");
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
        "preview wallpaper retains a varied multicolor field");
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
