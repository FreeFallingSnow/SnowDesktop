#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::icon_render_rules
{

constexpr int kMinimumSourcePixels = 64;
constexpr int kMaximumSourcePixels = 256;

/**
 * Select a bounded source-size bucket that is never smaller than the target.
 * Buckets avoid reloading every icon for one-pixel layout changes while still
 * keeping the cached bitmap substantially smaller than an unconditional
 * 256x256 allocation.
 */
constexpr int SourcePixelsForTarget(int targetPixels)
{
    const int target = std::clamp(
        targetPixels, kMinimumSourcePixels, kMaximumSourcePixels);
    if (target <= 64) return 64;
    if (target <= 96) return 96;
    if (target <= 128) return 128;
    if (target <= 192) return 192;
    return 256;
}

constexpr bool SourceLongEdgeCoversTarget(
    int sourceWidth, int sourceHeight, int targetPixels)
{
    return std::max(sourceWidth, sourceHeight) >=
        SourcePixelsForTarget(targetPixels);
}

/** Content/media thumbnails preserve their preview instead of receiving an icon plate. */
constexpr bool ShouldBeautify(bool beautificationEnabled, bool mediaThumbnail)
{
    return beautificationEnabled && !mediaThumbnail;
}

/** Full-quality loads always request folder previews; application files stay icon-only. */
constexpr bool ShouldRequestShellThumbnail(
    bool fullQualityPhase, bool applicationLike, bool shellFolder)
{
    return fullQualityPhase && (shellFolder || !applicationLike);
}

/** Folder previews remain beautifiable; only content/media thumbnails bypass beautification. */
constexpr bool IsMediaThumbnail(bool shellThumbnail, bool shellFolder)
{
    return shellThumbnail && !shellFolder;
}

struct FittedSize
{
    int width = 0;
    int height = 0;
};

/** Fit a bitmap inside a destination box without changing its aspect ratio or upscaling it. */
inline FittedSize FitWithoutUpscaling(
    int sourceWidth, int sourceHeight,
    int destinationWidth, int destinationHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        destinationWidth <= 0 || destinationHeight <= 0)
        return {};

    const double scale = std::min({
        1.0,
        static_cast<double>(destinationWidth) /
            static_cast<double>(sourceWidth),
        static_cast<double>(destinationHeight) /
            static_cast<double>(sourceHeight) });
    return {
        std::max(1, static_cast<int>(std::lround(sourceWidth * scale))),
        std::max(1, static_cast<int>(std::lround(sourceHeight * scale)))
    };
}

/**
 * Preserve the native pixel size of a window-provided HICON while bounding
 * larger sources to the requested Shell bucket. If Windows cannot report the
 * native size, keep the existing requested-size fallback.
 */
inline FittedSize WindowIconRasterSize(
    int nativeWidth, int nativeHeight, int requestedPixels)
{
    if (requestedPixels <= 0)
        return {};
    if (nativeWidth <= 0 || nativeHeight <= 0)
        return { requestedPixels, requestedPixels };
    return FitWithoutUpscaling(
        nativeWidth, nativeHeight,
        requestedPixels, requestedPixels);
}

} // namespace snowdesktop::icon_render_rules
