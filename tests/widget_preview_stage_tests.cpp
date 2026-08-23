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
    Check(dark.pixels != light.pixels,
        "dark and light wallpaper palettes remain distinct");
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
