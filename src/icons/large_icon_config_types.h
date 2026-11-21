#pragma once

#include "theme/edge_light_types.h"
#include "theme/panel_gradient_types.h"

#include <cstdint>
#include <string>

namespace snowdesktop
{
inline constexpr int kLargeIconSteamCover = 2;
inline constexpr int kLargeIconEpicCover = 3;

// An optional presentation of a Shell item. Runtime state and entitlement never
// belong here. Span remains the user's intent when a monitor temporarily shrinks.
struct LargeIconConfig
{
    int version = 3;
    int columns = 1, rows = 1;
    int shape = 0; // rounded rectangle, rounded square, circle, flag, diamond, hexagon
    int flagDirection = 0; // swallowtail notch: right, left, up, down
    bool regularHexagon = false; // Fit equal edges inside the grid allocation; old hexagons keep their proportions.
    double contentScale = .60;
    double fillScale = 1; // Independent of the foreground; relative to contain/cover fit.
    double radius = 12;
    double radiusPercent = -1; // -1 preserves the legacy/component CU radius; 100 is half the short edge.
    bool followComponentRadius = false; // Missing field preserves existing independent radii.
    bool showOnHoverOnly = false;
    bool keepWhenDesktopHidden = false;
    int content = 0; // 0 original, 1 imported static image, 2 Steam, 3 Epic
    int fit = 1; // Fill layer only: 0 contain, 1 cover. Foreground always contains.
    double focusX = .5, focusY = .5;
    std::string image;
    std::string imageAssetId;
    std::string cachedCover;
    bool autoColor = true;
    std::uint32_t manualColor = 0x505866;
    double colorMix = .65;
    double opacity = .65, hoverOpacity = .80;
    bool hoverOpacityLinked = true;
    bool border = true;
    std::uint32_t borderColor = 0xffffff;
    double borderWidth = 1, borderOpacity = .18;
    bool shadow = false;
    double shadowStrength = .2;
    int titleMode = 0; // 0 floating, 1 left reveal
    double titleSize = 12;
    double revealTitleSize = 24;
    bool autoTitleColor = true;
    std::uint32_t titleColor = 0xffffff;
    int hoverContent = 0; // 0 none, 1 left reveal, 2 zoom, 3 lift
    int hoverFrame = 1; // 0 none, 1 background, 2 border, 3 shadow
    bool press = true;
    int launch = 0; // 0 none, 1 jump, 2 pulse
    int coverHover = 0; // 0 none, 1 zoom, 2 title scrim
    double amplitude = 1;
    int delayMs = 120, enterMs = 200, exitMs = 160;
    int steamOrientation = 0; // Stored name for game cover orientation: 0 automatic, 1 landscape, 2 portrait
    bool localOnly = false;

    // v2: background selector is flat; nonnegative values are component preset
    // IDs (including 9/custom). Foreground and fill sources are independent.
    int backgroundStyle = -1; // -5 retired neutral, -4 icon plate, -3 legacy default, -2 image fill, -1 follow components
    // themeColor is the legacy storage name for the beautify-style background,
    // not an accent color. It is mutually exclusive with themeGradient.
    bool smartFill = true, themeColor = true, themeGradient = false;
    double themeOpacity = .65, themeAngle = 90;
    int defaultBackground = 0; // icon plate, editable gradient, editable solid
    std::uint32_t defaultSolidColor = 0xe8ecf4;
    double defaultSolidOpacity = .65;
    // disabled means derive colors from the icon until the first manual edit.
    PanelGradient defaultGradient;
    int foregroundContent = 0; // 0 original, 1 imported
    std::string foregroundImage;
    std::string foregroundImageAssetId;
    double iconX = .5, iconY = .5;
    int material = 0, componentTheme = 0; // plain/glass/acrylic; light/dark text
    double blurRadius = 24;
    bool edgeHighlight = false;
    double edgeWidth = 1, edgeStrength = .3;
    EdgeLightSettings edgeLight;
    PanelGradient gradient;
    double gradientOpacity = 1; // Independent multiplier; preserves individual stop opacity.
    int effect = 0; // none, 3D, dynamic title, gentle zoom, edge glow, one-shot shine
    double zoomAmount = .04, glowStrength = .45, shineStrength = .22;
    int shineDurationMs = 450;
    int titleDirection = 0; // left, up
    bool autoTitleDirection = true;
    int titleWeight = 600;

    friend bool operator==(const LargeIconConfig&, const LargeIconConfig&) = default;
};
}
