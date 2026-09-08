#include "icon_render_rules.h"
#include "large_icon_render_rules.h"
#include "large_icon_motion.h"
#include "icon_bitmap_pixels.h"
#include "icon_beautify.h"
#include "large_icon_transform.h"
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
    snowdesktop::LargeIconConfig config;
    snowdesktop::LargeIconMotion motion;
    Check(!motion.Advance(1000, true, true, true, 1, config) && motion.hover == 0,
        "no-effect mode never schedules hover animation");
    config.effect = 2;
    Check(motion.Advance(1000, true, true, true, 1, config) && motion.hover == 0, "title honors initial delay");
    motion.Advance(1071, true, true, true, 1, config);
    Check(motion.hover == 0, "title never appears before its delay");
    motion.Advance(1132, true, true, true, 1, config);
    Check(std::abs(motion.hover - .5f) < .0001, "title reaches transition midpoint");
    motion.Advance(1132, false, true, true, 1, config);
    Check(std::abs(motion.hover - .5f) < .0001, "rapid exit preserves current pose");
    motion.Advance(1180, false, true, true, 1, config);
    Check(std::abs(motion.hover - .25f) < .0001, "exit uses independent duration");
    motion.Advance(1180, true, true, true, 1, config);
    Check(!motion.Advance(1500, true, true, true, 1, config) && motion.hover == 1, "settled title stops animation frames");
    Check(!motion.Advance(1501, true, false, true, 1, config) && motion.hover == 0, "hidden or blocked items immediately reset");
    Check(!motion.Advance(1502, true, true, false, 1, config) && motion.hover == 1, "reduced motion shows inner title instantly");
    config.effect = 1;
    Check(!motion.Advance(1503, true, true, false, 1, config) && motion.hover == 0, "reduced motion disables 3D");
    Check(motion.AdvanceTilt(1600, 1, -1, true, true, true, 1, config), "pointer change starts a finite tilt");
    motion.AdvanceTilt(1632.5, -1, 1, true, true, true, 1, config);
    Check(std::abs(motion.tiltX - .875f) < .0001 && std::abs(motion.tiltY + .875f) < .0001,
        "tilt reaches most of its target within 33ms and preserves pose on reversal");
    snowdesktop::LargeIconMotion following;
    following.Advance(2000, true, true, true, 1, config);
    Check(following.hover == 1, "3D does not multiply pointer tracking by a delayed hover fade");
    following.AdvanceTilt(2000, 1, 0, true, true, true, 1, config);
    for (int i = 1; i <= 6; ++i) following.AdvanceTilt(2000 + i * 8, 1 - i * .02f, 0, true, true, true, 1, config);
    Check(following.tiltX > .75f, "continuous high-rate pointer events do not restart a slow ease-in");
    Check(!motion.AdvanceTilt(1900, -1, 1, true, true, true, 1, config) && motion.tiltX == -1,
        "settled pointer tilt does not request idle frames");
    Check(!motion.AdvanceTilt(1901, -1, 1, true, false, true, 1, config) && motion.tiltX == 0,
        "hidden or blocked cards discard pointer motion");
    const auto tilt = snowdesktop::large_icon_transform::Resolve(400, 180, 1, -.8f, 1);
    const auto center = snowdesktop::large_icon_transform::Project(tilt.matrix, 200, 90);
    Check(tilt.active && std::abs(center.x - 200) < .001 && std::abs(center.y - 90) < .001,
        "shared backdrop/content transform preserves the card center");
    Check(!snowdesktop::large_icon_transform::Resolve(400, 180, 1, 1, 0).active, "zero-strength 3D skips effect rendering");
    config = {};
    const MeasureTitle measured = [](double w, double h) { return TitleSize{96, 32, w >= 96 && h >= 32}; };
    auto geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, 0, measured);
    Check(geometry.width == 32 && geometry.x == 184 && geometry.y == 74, "small original stays centered without upscaling");
    config.iconX = 0; config.iconY = 1;
    geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, 0, measured);
    Check(geometry.x == 0 && geometry.y == 148, "foreground positions use the available travel on each axis");
    config.effect = 2;
    geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, 0, measured);
    Check(geometry.x == 184 && geometry.y == 74, "dynamic title overrides saved foreground position at rest");
    geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, 1, measured);
    Check(geometry.leftReveal && geometry.x == 130 && geometry.width == 32 && geometry.titleLeft == 174 && geometry.titleWidth == 96,
        "left reveal keeps original size and centers text in the remaining right column");
    Check(CanSelectTitleDirection(config, 0, 180, 400, 64, 64, 1, measured) && CanSelectTitleDirection(config, 1, 180, 400, 64, 64, 1, measured),
        "compact spacing permits a title when the actual icon and text fit");
    auto largeImage = config; largeImage.contentScale = 1;
    auto fullHeight = ResolveContent(largeImage, 400, 180, 256, 256, 1, true, 1, measured);
    Check(fullHeight.leftReveal && fullHeight.height == 180 && fullHeight.titleWidth == 96,
        "a full-height foreground still reveals text to its right without shrinking the icon");
    auto fullWidth = ResolveContent(largeImage, 180, 400, 256, 256, 1, true, 1, measured);
    Check(fullWidth.upReveal && fullWidth.width == 180,
        "a full-width foreground still permits the lower title");
    Check(!CanSelectTitleDirection(config, 0, 60, 60, 64, 64, 1, measured) && !CanSelectTitleDirection(config, 1, 60, 60, 64, 64, 1, measured),
        "insufficient frames cannot enable either direction by shrinking content");
    config.titleDirection = 1;
    geometry = ResolveContent(config, 180, 400, 64, 64, 1, true, 1, measured);
    Check(geometry.upReveal && geometry.y == 146 && geometry.width == 64 && geometry.titleTop == 222,
        "up direction reserves a lower text area without resizing the icon");
    config.backgroundStyle = -2; config.titleDirection = 0; config.focusX = 1;
    geometry = ResolveContent(config, 400, 180, 800, 200, 1, true, 1, measured);
    Check(!geometry.leftReveal && !geometry.upReveal && geometry.cropped && geometry.width == 400 && geometry.x == 0,
        "saved dynamic-title settings never translate image-fill content");
    Check(!motion.Advance(2000, true, true, true, 1, config) && motion.hover == 0,
        "image fill schedules no dynamic-title animation");
    config.effect = 0; config.fit = 0;
    geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, 0, measured);
    Check(geometry.width == 180 && geometry.x == 110, "fill original is enlarged independently of foreground limits");
    config.fillScale = .5;
    geometry = ResolveContent(config, 400, 180, 32, 32, 1, true, 0);
    Check(geometry.width == 90 && geometry.x == 155 && geometry.y == 45, "fill scale can shrink the image independently of foreground scale");
    config.fit = 1; config.fillScale = 2;
    geometry = ResolveContent(config, 400, 180, 800, 200, 1, false, 0);
    Check(geometry.width == 400 && geometry.sourceWidth < 400 && geometry.sourceHeight < 200,
        "zooming cover creates crop room on both axes without growing the frame");
    config.effect = 2;
    const auto leftZoom = ResolveContent(config, 400, 180, 800, 200, 1, false, 1, measured);
    Check(!leftZoom.leftReveal && !leftZoom.upReveal && leftZoom.x == 0 && leftZoom.sourceWidth == geometry.sourceWidth,
        "zoomed image fill retains its crop with dynamic title disabled");
    config = {};
    Check(Radius(config, 200, 100, 2) == 24 && RadiusPercent(config, 200, 100, 2) == 48, "legacy radius preserves CU geometry");
    config.radiusPercent = 100;
    Check(Radius(config, 200, 100, 1) == 50 && Radius(config, 400, 200, 2) == 100, "100 percent radius follows half short edge at each DPI");
    using snowdesktop::large_icon_settings_rules::Field;
    using snowdesktop::large_icon_settings_rules::Visible;
    using snowdesktop::large_icon_settings_rules::Enabled;
    config = {};
    Check(Visible(Field::Default, config) && !Visible(Field::Smart, config) && Visible(Field::Smart, config, true) &&
        !Visible(Field::Fill, config) && !Visible(Field::Solid, config), "default exposes contour only when detected and never manual color or crop");
    Check(!Enabled(Field::ThemeOptions, config, true, true) && Enabled(Field::ThemeOptions, config, false, true),
        "theme fallback parameters disable while opaque contour takes precedence");
    config.backgroundStyle = -2; config.content = 2;
    Check(Visible(Field::Steam, config) && Visible(Field::Crop, config) && !Visible(Field::Foreground, config) && !Visible(Field::Custom, config),
        "Steam fill has its own settings and no foreground or component material");
    config.content = 1; config.fit = 0;
    Check(Visible(Field::FillImage, config) && !Visible(Field::Steam, config) && !Visible(Field::Crop, config), "local contain hides Steam and crop settings");
    config.backgroundStyle = 9; config.gradient.enabled = true; config.effect = 2; config.autoTitleColor = false;
    Check(Visible(Field::Custom, config) && !Visible(Field::Solid, config) && Visible(Field::ManualTitle, config) &&
        !Enabled(Field::ForegroundPosition, config, false, true), "custom gradient suppresses duplicate solid parameters and dynamic title owns position");
    config = {};
    auto background = DefaultBackground(config, 0, false, 0);
    Check(background.color == 0xe8ecf4 && background.opacity == .65, "first frame and failed extraction have the opaque default-beautify base");
    background = DefaultBackground(config, 0x008800, true, 0x0112ff);
    Check(background.color == 0x0112ff && background.opacity == 1 && !background.gradient.enabled, "reliable contour is preserved exactly and filled opaquely");
    config.smartFill = false;
    background = DefaultBackground(config, 0x008800, true, 0x0112ff);
    Check(background.color == 0xe8ecf4 && background.opacity == .65 && !background.gradient.enabled,
        "plain icon background falls back to the beautify base, never an accent fill");
    config.smartFill = false; config.defaultBackground = 1; config.themeOpacity = .4; config.themeAngle = 45;
    background = DefaultBackground(config, 0x008800, true, 0x0112ff);
    Check(background.color == 0x008800 && background.opacity == .4 && background.gradient.enabled && background.gradient.angle == 45,
        "theme fallback supports opacity and automatic gradient direction without manual colors");
    Check(Visible(Field::ThemeGradient, config) && Visible(Field::EditableBackground, config) && Enabled(Field::ThemeGradient, config, false, false),
        "default gradient keeps the complete editor available without image samples");
    config.defaultGradient.enabled = true; config.defaultGradient.angle = 213;
    config.defaultGradient.stops = {{0, 0x112233, .2}, {.3, 0x998877, .4}, {1, 0xabcdef, .8}};
    background = DefaultBackground(config, 0, false, 0);
    Check(background.gradient == config.defaultGradient, "manual default gradient retains every stop, opacity and direction without theme samples");
    config.defaultBackground = 2; config.defaultSolidColor = 0x1133ff; config.defaultSolidOpacity = .37;
    background = DefaultBackground(config, 0xff0000, true, 0x008800);
    Check(background.color == 0x1133ff && background.opacity == .37 && !background.gradient.enabled &&
        Visible(Field::DefaultSolid, config) && Visible(Field::EditableBackground, config),
        "default solid exposes color and opacity and ignores automatic contour/gradient choices");
    config.backgroundStyle = -2; config.effect = 2;
    Check(!Visible(Field::Title, config) && !Visible(Field::ManualTitle, config), "image fill hides unsupported title controls");
    std::vector<std::uint32_t> straight{0x8000c864, 0xff00c864, 0x0000ffff};
    snowdesktop::icon_bitmap_pixels::NormalizeShellPixels(straight);
    Check(straight[0] == 0x80006432 && straight[1] == 0xff00c864 && straight[2] == 0,
        "straight Shell alpha is premultiplied before PNG encoding instead of producing cyan fringes");
    const auto normalized = straight;
    snowdesktop::icon_bitmap_pixels::NormalizeShellPixels(straight);
    Check(straight == normalized, "already-premultiplied icon samples are not darkened again");
    std::vector<unsigned> rounded(96 * 96);
    for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x)
    {
        const double dx = std::max({20. - x, 0., x - 75.}), dy = std::max({20. - y, 0., y - 75.});
        const auto a = static_cast<unsigned>(255 * std::clamp(16.5 - std::sqrt(dx * dx + dy * dy), 0., 1.));
        rounded[y * 96 + x] = a << 24 | ((32 * a / 255) << 16) | ((200 * a / 255) << 8) | (112 * a / 255);
    }
    for (int y = 32; y < 64; ++y) for (int x = 32; x < 64; ++x) rounded[y * 96 + x] = 0xffffffff;
    const auto contour = snowdesktop::icon_beautify::DetectPlateFill(rounded, 96, 96);
    Check(contour && contour->r == 32 && contour->g == 200 && contour->b == 112,
        "visible rounded contour survives transparent image margins and an opaque contrasting logo");
    for (int y = 36; y < 60; ++y) for (int x = 36; x < 60; ++x) rounded[y * 96 + x] = 0;
    Check(!snowdesktop::icon_beautify::DetectPlateFill(rounded, 96, 96), "a hollow icon silhouette is not mistaken for a solid background plate");
    config.backgroundStyle = 7;
    Check(TextColor(config, 0x111111, 0x161616) == 0x161616, "component title color uses theme foreground even when luminance disagrees");
    config.autoTitleColor = false; config.titleColor = 0x123456;
    Check(TextColor(config, 0xffffff, 0xffffff) == 0x123456, "manual title color overrides theme and luminance");
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
