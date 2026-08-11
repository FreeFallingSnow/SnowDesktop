#include "icon_render_rules.h"

#include <iostream>
#include <vector>

namespace rules = snowdesktop::icon_render_rules;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

std::uint32_t PremultipliedPixel(
    int red, int green, int blue, int alpha = 255)
{
    const auto premultiply = [alpha](int channel)
    {
        return static_cast<std::uint32_t>(
            (channel * alpha + 127) / 255);
    };
    return (static_cast<std::uint32_t>(alpha) << 24) |
        (premultiply(red) << 16) |
        (premultiply(green) << 8) |
        premultiply(blue);
}

void PaintRing(std::vector<std::uint32_t>& pixels,
    int size, int inset, std::uint32_t color)
{
    for (int x = inset; x < size - inset; ++x)
    {
        pixels[static_cast<size_t>(inset) * size + x] = color;
        pixels[static_cast<size_t>(size - 1 - inset) * size + x] = color;
    }
    for (int y = inset; y < size - inset; ++y)
    {
        pixels[static_cast<size_t>(y) * size + inset] = color;
        pixels[static_cast<size_t>(y) * size + size - 1 - inset] = color;
    }
}
} // namespace

int main()
{
    Check(rules::SourcePixelsForTarget(32) == 64,
        "small icons use the baseline source bucket");
    Check(rules::SourcePixelsForTarget(65) == 96,
        "a target just above 64 pixels must never reuse a 64-pixel source");
    Check(rules::SourcePixelsForTarget(97) == 128,
        "source buckets must cover their target");
    Check(rules::SourcePixelsForTarget(129) == 192,
        "large targets use the 192-pixel bucket");
    Check(rules::SourcePixelsForTarget(193) == 256,
        "very large targets use the JUMBO-sized bucket");
    Check(rules::SourcePixelsForTarget(400) == 256,
        "source allocation is bounded at the Shell JUMBO size");

    Check(!rules::SourceLongEdgeCoversTarget(64, 64, 65),
        "a 64-pixel bitmap cannot cover a larger target");
    Check(rules::SourceLongEdgeCoversTarget(96, 64, 65),
        "a non-square thumbnail is covered by its long edge");

    const auto downscaled = rules::FitWithoutUpscaling(96, 96, 65, 65);
    Check(downscaled.width == 65 && downscaled.height == 65,
        "large square sources fit the requested target exactly");
    const auto noUpscale = rules::FitWithoutUpscaling(64, 64, 96, 96);
    Check(noUpscale.width == 64 && noUpscale.height == 64,
        "undersized sources are never enlarged");
    const auto aspectFit = rules::FitWithoutUpscaling(96, 64, 65, 65);
    Check(aspectFit.width == 65 && aspectFit.height == 43,
        "non-square thumbnails keep their aspect ratio");

    constexpr int artifactSize = 128;
    const std::uint32_t grayFrame = PremultipliedPixel(128, 128, 128, 176);
    const std::uint32_t artwork = PremultipliedPixel(30, 100, 220);
    std::vector<std::uint32_t> framed(
        static_cast<size_t>(artifactSize) * artifactSize, 0);
    PaintRing(framed, artifactSize, 0, grayFrame);
    for (int y = 44; y < 84; ++y)
    {
        for (int x = 44; x < 84; ++x)
            framed[static_cast<size_t>(y) * artifactSize + x] = artwork;
    }
    Check(rules::SuppressOuterFrameArtifact(
            framed.data(), artifactSize, artifactSize),
        "a continuous neutral frame around transparent icon content is suppressed");
    Check(framed.front() == 0 &&
            framed[static_cast<size_t>(64) * artifactSize + 64] == artwork,
        "frame suppression clears the artifact without changing central artwork");

    std::vector<std::uint32_t> insetFrame(
        static_cast<size_t>(artifactSize) * artifactSize, 0);
    PaintRing(insetFrame, artifactSize, 1, grayFrame);
    PaintRing(insetFrame, artifactSize, 2, grayFrame);
    Check(rules::SuppressOuterFrameArtifact(
            insetFrame.data(), artifactSize, artifactSize),
        "an inset two-pixel neutral frame is suppressed");
    Check(insetFrame[static_cast<size_t>(1) * artifactSize + 64] == 0 &&
            insetFrame[static_cast<size_t>(2) * artifactSize + 64] == 0,
        "all detected frame bands are cleared");

    std::vector<std::uint32_t> fullBleed(
        static_cast<size_t>(artifactSize) * artifactSize, grayFrame);
    Check(!rules::SuppressOuterFrameArtifact(
            fullBleed.data(), artifactSize, artifactSize),
        "a full-bleed neutral square is not mistaken for a frame artifact");

    std::vector<std::uint32_t> coloredBorder(
        static_cast<size_t>(artifactSize) * artifactSize, 0);
    PaintRing(coloredBorder, artifactSize, 0, artwork);
    Check(!rules::SuppressOuterFrameArtifact(
            coloredBorder.data(), artifactSize, artifactSize),
        "a saturated application border is preserved");

    std::vector<std::uint32_t> partialBorder(
        static_cast<size_t>(artifactSize) * artifactSize, 0);
    for (int x = 0; x < artifactSize; ++x)
        partialBorder[static_cast<size_t>(x)] = grayFrame;
    Check(!rules::SuppressOuterFrameArtifact(
            partialBorder.data(), artifactSize, artifactSize),
        "a one-sided neutral shadow does not trigger frame suppression");

    constexpr int smallSize = 96;
    std::vector<std::uint32_t> smallFrame(
        static_cast<size_t>(smallSize) * smallSize, 0);
    PaintRing(smallFrame, smallSize, 0, grayFrame);
    Check(!rules::SuppressOuterFrameArtifact(
            smallFrame.data(), smallSize, smallSize),
        "low-resolution icon layers are outside the suppression scope");

    if (failures != 0)
    {
        std::cerr << failures << " icon render rule test(s) failed\n";
        return 1;
    }
    std::cout << "Icon render rule tests passed\n";
    return 0;
}
