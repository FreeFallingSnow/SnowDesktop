#include "icon_render_rules.h"

#include <iostream>

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

    Check(rules::ShouldBeautify(true, false),
        "ordinary icons participate when beautification is enabled");
    Check(!rules::ShouldBeautify(true, true),
        "content/media thumbnails bypass icon beautification");
    Check(!rules::ShouldBeautify(false, false),
        "ordinary icons remain untouched when beautification is disabled");

    Check(rules::ShouldRequestShellThumbnail(true, false, false),
        "full-quality non-application loads may request a Shell thumbnail");
    Check(!rules::ShouldRequestShellThumbnail(true, true, false),
        "application-like items keep their ordinary icon representation");
    Check(rules::ShouldRequestShellThumbnail(true, true, true),
        "folders request thumbnails even when their names look application-like");
    Check(!rules::ShouldRequestShellThumbnail(false, false, true),
        "the fast first phase never requests thumbnails");
    Check(rules::IsMediaThumbnail(true, false),
        "content thumbnails bypass beautification");
    Check(!rules::IsMediaThumbnail(true, true),
        "folder thumbnails remain eligible for beautification");
    Check(!rules::IsMediaThumbnail(false, false),
        "ordinary icons are not classified as media thumbnails");

    const auto downscaled = rules::FitWithoutUpscaling(96, 96, 65, 65);
    Check(downscaled.width == 65 && downscaled.height == 65,
        "large square sources fit the requested target exactly");
    const auto noUpscale = rules::FitWithoutUpscaling(64, 64, 96, 96);
    Check(noUpscale.width == 64 && noUpscale.height == 64,
        "undersized sources are never enlarged");
    const auto aspectFit = rules::FitWithoutUpscaling(96, 64, 65, 65);
    Check(aspectFit.width == 65 && aspectFit.height == 43,
        "non-square thumbnails keep their aspect ratio");

    if (failures != 0)
    {
        std::cerr << failures << " icon render rule test(s) failed\n";
        return 1;
    }
    std::cout << "Icon render rule tests passed\n";
    return 0;
}
