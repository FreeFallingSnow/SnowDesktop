#pragma once

#include <d2d1_1.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <vector>

namespace snowdesktop::widget_preview
{

struct Wallpaper
{
    int width = 0;
    int height = 0;
    /// Premultiplied BGRA pixels, stored top-down. Every pixel is opaque.
    std::vector<std::uint32_t> pixels;
};

struct StageStyle
{
    /// Retained for material compatibility; the shared landscape is invariant.
    bool lightTheme = false;
    bool glassEnabled = false;
    float blurRadius = 0.0f;
    float cornerRadius = 0.0f;
};

/** Pixel-space crop within a larger deterministic wallpaper composition. */
struct WallpaperViewport
{
    int canvasWidth = 0;
    int canvasHeight = 0;
    int offsetX = 0;
    int offsetY = 0;
};

inline constexpr std::size_t AcrylicNoiseSize = 64;
using AcrylicNoisePixels = std::array<std::uint32_t,
    AcrylicNoiseSize * AcrylicNoiseSize>;

/** Generate the deterministic shared preview landscape. */
Wallpaper GenerateWallpaper(int width, int height, bool lightTheme);

/** Generate an exact crop from a larger wallpaper composition. */
Wallpaper GenerateWallpaper(int width, int height, bool lightTheme,
    const WallpaperViewport& viewport);

/** Generate the same fixed acrylic texture used by live widget panels. */
AcrylicNoisePixels GenerateAcrylicNoise(bool lightTheme);

/** Draw the sharp wallpaper and, when requested, its clipped blurred layer. */
bool DrawStage(ID2D1DeviceContext* context, const RECT& bounds,
    const StageStyle& style, const WallpaperViewport& viewport = {});

/** Draw a one-shot acrylic texture for the out-of-process author preview. */
void DrawAcrylicNoise(ID2D1DeviceContext* context, const RECT& bounds,
    float cornerRadius, bool lightTheme, POINT pixelOrigin = {});

/** Draw the shared liquid-glass edge treatment. */
bool DrawGlassBorder(ID2D1DeviceContext* context, const RECT& bounds,
    float cornerRadius, D2D1_COLOR_F color, float strokeWidth);

} // namespace snowdesktop::widget_preview
