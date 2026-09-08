#include "icon_render_rules.h"
#include "large_icon_render_rules.h"
#include "large_icon_motion.h"
#include "large_icon_settings_rules.h"

#include <iostream>

namespace rules = snowdesktop::icon_render_rules;
int RunLargeIconAssetTests();
int RunLargeIconShellAssetTests();
int RunLargeIconRenderingTests(const char* outputDirectory);

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

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--large-icon-shell") return RunLargeIconShellAssetTests();
    if (argc >= 2 && std::string_view(argv[1]) == "--large-icon-rendering") return RunLargeIconRenderingTests(argc == 3 ? argv[2] : nullptr);
    failures += RunLargeIconAssetTests();
    using namespace snowdesktop::large_icon_render_rules;
    Check(CanRevealTitle(400, 180, 108, 12, 1), "wide large icons can reveal names without resizing content");
    Check(!CanRevealTitle(100, 400, 60, 12, 1), "portrait frames keep floating titles");
    Check(!CanRevealTitle(180, 100, 100, 18, 1), "insufficient text room keeps floating titles");
    Check(TitleBackdrop(0) != TitleBackdrop(0xffffff), "manual dark titles receive a readable light backdrop");
    snowdesktop::LargeIconConfig config;
    snowdesktop::LargeIconMotion motion;
    Check(motion.Advance(1000, true, true, true, 1, config) && motion.hover == 0, "hover starts with the configured delay");
    motion.Advance(1119, true, true, true, 1, config);
    Check(motion.hover == 0, "hover delay does not reveal the title early");
    motion.Advance(1220, true, true, true, 1, config);
    Check(std::abs(motion.hover - .5f) < .0001, "hover reaches the midpoint after half the entry duration");
    motion.Advance(1220, false, true, true, 1, config);
    Check(std::abs(motion.hover - .5f) < .0001, "rapid exit reverses from the current pose without jumping");
    motion.Advance(1300, false, true, true, 1, config);
    Check(std::abs(motion.hover - .25f) < .0001, "exit uses its independent duration");
    motion.Advance(1300, true, true, true, 1, config);
    Check(std::abs(motion.hover - .25f) < .0001, "reentry continues without repeating the initial delay");
    Check(!motion.Advance(1500, true, true, true, 1, config) && motion.hover == 1, "settled hover requests no animation frames");
    Check(!motion.Advance(1501, true, false, true, 1, config) && motion.hover == 0, "hidden or blocked items immediately stop and clear effects");
    Check(!motion.Advance(1502, true, true, false, 1, config) && motion.hover == 1, "reduced animation reveals the name without a transition");
    config.enterMs = 0; motion = {};
    motion.Advance(2000, true, true, true, 1, config);
    Check(motion.hover == 0, "zero-duration transitions still honor the hover delay");
    Check(!motion.Advance(2120, true, true, true, 1, config) && motion.hover == 1, "zero-duration transition completes once after the delay");
    config.launch = 1; motion.Launch(3000, true, config);
    Check(std::abs(motion.LaunchWave(3450, 2) - 1) < .0001, "launch feedback follows the global duration scale");
    Check(!motion.Advance(3900, true, true, true, 2, config) && motion.launchStart == 0 && motion.LaunchWave(3901, 2) == 0,
        "launch feedback finishes once and does not leave a recurring refresh");
    motion.Launch(4000, true, config);
    motion.Advance(4001, true, true, false, 1, config);
    Check(motion.launchStart == 0, "disabling animations cancels in-flight launch feedback");

    config = {};
    auto geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, true, 0, false, 0);
    Check(geometry.width == 32 && geometry.x == 184 && geometry.y == 74, "small original pixels remain unscaled and exactly centered");
    config.hoverContent = 2;
    auto zoomed = ResolveContent(config, 400, 180, 32, 32, 1, true, true, 1, false, 0);
    Check(std::abs(zoomed.width - 32 * 1.06) < .0001 && !zoomed.leftReveal, "hover enlargement also affects small original icons");
    config.titleMode = 1; config.hoverContent = 1;
    geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, true, 1, false, 0);
    Check(geometry.leftReveal && geometry.x == 64 && geometry.width == 32 && geometry.titleLeft == 172 && geometry.titleWidth == 216,
        "left reveal centers the unchanged original in the left column and reserves the right for large text");
    geometry = ResolveContent(config, 180, 400, 256, 256, 1, true, true, 1, false, 0);
    Check(!geometry.leftReveal && geometry.x == 36 && geometry.width == 108, "portrait fallback does not shrink the icon to fit text");
    geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, false, 1, false, 0);
    Check(!geometry.leftReveal && geometry.x == 184, "reduced motion keeps the original centered with a floating title");
    config.content = 1; config.fit = 1;
    geometry = ResolveContent(config, 200, 100, 400, 200, 1, false, true, 0, true, 0);
    Check(!geometry.leftReveal && geometry.width == 192 && geometry.height == 96 && geometry.x == 4 && geometry.y == 2,
        "pressing an exact-aspect cover visibly shrinks it inside the fixed frame");
    config.focusX = 1;
    geometry = ResolveContent(config, 100, 200, 400, 200, 1, false, true, 0, false, 0);
    Check(geometry.x == -300 && geometry.width == 400 && geometry.height == 200, "fill focus crops without stretching the source");
    config = {};
    Check(Radius(config, 200, 100, 2) == 24 && RadiusPercent(config, 200, 100, 2) == 48,
        "legacy component radius keeps its CU geometry and displays a normalized percentage");
    config.radiusPercent = 100;
    Check(Radius(config, 200, 100, 1) == 50 && Radius(config, 400, 200, 2) == 100,
        "maximum relative radius follows half the short edge through resize and DPI changes");
    config.radiusPercent = 50;
    Check(Radius(config, 300, 100, 1) == 25, "relative rounding uses the short edge, not frame width");
    config.radiusPercent = 0;
    Check(Radius(config, 200, 100, 2) == 0, "zero relative radius overrides the retained legacy fallback");
    using snowdesktop::large_icon_settings_rules::Field;
    using snowdesktop::large_icon_settings_rules::Visible;
    using snowdesktop::large_icon_settings_rules::CanSelectLeftTitle;
    config = {};
    Check(!Visible(Field::Image, config) && !Visible(Field::Crop, config) && !Visible(Field::Imported, config) &&
        !Visible(Field::Steam, config) && !Visible(Field::ManualBackground, config) && !Visible(Field::ManualTitle, config),
        "original and automatic modes hide unsupported media and manual color editors");
    config.content = 1;
    Check(Visible(Field::Imported, config) && Visible(Field::Image, config) && !Visible(Field::Original, config) && !Visible(Field::Crop, config),
        "contain images expose import and fit but hide crop focus and original scaling");
    config.fit = 1; config.content = 2; config.autoColor = false; config.autoTitleColor = false;
    Check(Visible(Field::Crop, config) && Visible(Field::Steam, config) && !Visible(Field::Imported, config) &&
        Visible(Field::ManualBackground, config) && !Visible(Field::AutomaticBackground, config) && Visible(Field::ManualTitle, config),
        "Steam fill and manual modes reveal only their applicable child settings");
    config.border = false; config.shadow = false; config.hoverFrame = 0;
    Check(!Visible(Field::BorderAppearance, config) && !Visible(Field::BorderOpacity, config) && !Visible(Field::ShadowStrength, config) &&
        !Visible(Field::HoverBackground, config), "disabled frame effects hide unused appearance settings");
    config.hoverFrame = 2;
    Check(Visible(Field::BorderAppearance, config) && !Visible(Field::BorderOpacity, config),
        "hover-only border exposes its color and width without claiming a normal border opacity");
    config = {}; config.titleMode = 1;
    Check(Visible(Field::RevealTitle, config) && !Visible(Field::OriginalMotion, config) &&
        CanSelectLeftTitle(config, 400, 180, 64, 64, 1, true) &&
        !CanSelectLeftTitle(config, 180, 400, 64, 64, 1, true) &&
        !CanSelectLeftTitle(config, 400, 180, 64, 64, 1, false),
        "left-title editor follows production space and animation eligibility without a second movement selector");
    config.content = 2;
    Check(!CanSelectLeftTitle(config, 400, 180, 64, 64, 1, true), "covers cannot select the original-icon left title effect");
    config = {};
    Check(Background(config, 0x414751, 0x414751) == 0x414751, "missing color data immediately uses the neutral background");
    Check(Background(config, 0xc9ced6, 0) == 0xc9ced6 && Background(config, 0x414751, 0) == 0x414751,
        "fully transparent sources follow both light and dark theme neutral colors");
    config.autoColor = false; config.manualColor = 0x123456;
    Check(Background(config, 0, 0xffffff) == 0x123456, "manual colors remain independent from automatic extraction");
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
