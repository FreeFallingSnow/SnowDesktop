#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

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

namespace detail
{

constexpr std::uint8_t PixelAlpha(std::uint32_t pixel)
{
    return static_cast<std::uint8_t>(pixel >> 24);
}

inline int StraightChannel(std::uint32_t pixel, int shift)
{
    const int alpha = PixelAlpha(pixel);
    if (alpha <= 0)
        return 0;
    const int channel = static_cast<int>((pixel >> shift) & 0xffu);
    return std::clamp((channel * 255 + alpha / 2) / alpha, 0, 255);
}

inline bool IsNeutralFramePixel(std::uint32_t pixel, bool relaxed)
{
    const int alpha = PixelAlpha(pixel);
    if (alpha < (relaxed ? 8 : 16))
        return false;

    const int blue = StraightChannel(pixel, 0);
    const int green = StraightChannel(pixel, 8);
    const int red = StraightChannel(pixel, 16);
    const int minimum = std::min({ red, green, blue });
    const int maximum = std::max({ red, green, blue });
    const int luminance = (red * 54 + green * 183 + blue * 19) / 256;
    return maximum - minimum <= (relaxed ? 30 : 18) &&
        luminance >= (relaxed ? 20 : 32) &&
        luminance <= (relaxed ? 245 : 235);
}

struct RingCoverage
{
    std::array<int, 4> candidates{};
    std::array<int, 4> totals{};

    int CandidateCount() const
    {
        return candidates[0] + candidates[1] +
            candidates[2] + candidates[3];
    }

    int TotalCount() const
    {
        return totals[0] + totals[1] + totals[2] + totals[3];
    }

    bool IsStrongFrame() const
    {
        for (size_t side = 0; side < candidates.size(); ++side)
        {
            if (totals[side] <= 0 ||
                candidates[side] * 100 < totals[side] * 65)
                return false;
        }
        return CandidateCount() * 100 >= TotalCount() * 72;
    }

    bool ContinuesFrame() const
    {
        for (size_t side = 0; side < candidates.size(); ++side)
        {
            if (totals[side] <= 0 ||
                candidates[side] * 100 < totals[side] * 20)
                return false;
        }
        return CandidateCount() * 100 >= TotalCount() * 35;
    }
};

inline RingCoverage MeasureRing(const std::uint32_t* pixels,
    int width, int height, int inset)
{
    RingCoverage result;
    const int left = inset;
    const int right = width - 1 - inset;
    const int top = inset;
    const int bottom = height - 1 - inset;
    if (!pixels || left >= right || top >= bottom)
        return result;

    const auto addPixel = [&](int side, int x, int y)
    {
        ++result.totals[side];
        if (IsNeutralFramePixel(
                pixels[static_cast<size_t>(y) * width + x], false))
            ++result.candidates[side];
    };

    for (int x = left; x <= right; ++x)
    {
        addPixel(0, x, top);
        addPixel(1, x, bottom);
    }
    for (int y = top + 1; y < bottom; ++y)
    {
        addPixel(2, left, y);
        addPixel(3, right, y);
    }
    return result;
}

inline bool HasTransparentInterior(const std::uint32_t* pixels,
    int width, int height, int inset)
{
    const int left = inset;
    const int right = width - inset;
    const int top = inset;
    const int bottom = height - inset;
    if (!pixels || left >= right || top >= bottom)
        return false;

    int transparent = 0;
    int total = 0;
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            ++total;
            if (PixelAlpha(pixels[static_cast<size_t>(y) * width + x]) <= 8)
                ++transparent;
        }
    }
    return total > 0 && transparent * 100 >= total * 8;
}

} // namespace detail

/**
 * Remove a thin, neutral rectangular ring emitted at the canvas edge of some
 * high-resolution application icon layers. The detector deliberately requires
 * all four sides, transparent content inside the ring, and a discontinuity on
 * the next inner ring so that ordinary full-bleed square icons are preserved.
 */
inline bool SuppressOuterFrameArtifact(
    std::uint32_t* pixels, int width, int height)
{
    if (!pixels || width < 128 || height < 128)
        return false;

    constexpr int kMaximumStartInset = 3;
    const int maximumThickness = std::clamp(
        std::min(width, height) / 64, 1, 4);
    for (int start = 0; start <= kMaximumStartInset; ++start)
    {
        const detail::RingCoverage outer =
            detail::MeasureRing(pixels, width, height, start);
        if (!outer.IsStrongFrame())
            continue;

        int thickness = 1;
        while (thickness < maximumThickness)
        {
            const detail::RingCoverage next = detail::MeasureRing(
                pixels, width, height, start + thickness);
            if (!next.ContinuesFrame())
                break;
            ++thickness;
        }

        const int interiorInset = start + thickness;
        if (detail::MeasureRing(pixels, width, height, interiorInset)
                .ContinuesFrame() ||
            !detail::HasTransparentInterior(
                pixels, width, height, interiorInset))
            continue;

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int edgeDistance = std::min({
                    x, y, width - 1 - x, height - 1 - y });
                if (edgeDistance < start ||
                    edgeDistance >= start + thickness)
                    continue;
                std::uint32_t& pixel =
                    pixels[static_cast<size_t>(y) * width + x];
                if (detail::IsNeutralFramePixel(pixel, true))
                    pixel = 0;
            }
        }
        return true;
    }
    return false;
}

} // namespace snowdesktop::icon_render_rules
