#pragma once

#include "json_value.h"
#include "panel_gradient.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <type_traits>
#include <string>
#include <string_view>

namespace snowdesktop
{
// An optional presentation of a Shell item. Runtime state and entitlement never
// belong here. Span remains the user's intent when a monitor temporarily shrinks.
struct LargeIconConfig
{
    int version = 2;
    int columns = 2, rows = 2;
    double contentScale = .60;
    double fillScale = 1; // Independent of the foreground; relative to contain/cover fit.
    double radius = 12;
    double radiusPercent = -1; // -1 preserves the legacy/component CU radius; 100 is half the short edge.
    int content = 0; // 0 original, 1 imported static image, 2 Steam
    int fit = 1; // Fill layer only: 0 contain, 1 cover. Foreground always contains.
    double focusX = .5, focusY = .5;
    std::string image;
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
    int steamOrientation = 0; // 0 automatic, 1 landscape, 2 portrait
    bool localOnly = false;

    // v2: background selector is flat; nonnegative values are component preset
    // IDs (including 9/custom). Foreground and fill sources are independent.
    int backgroundStyle = -3; // -3 default, -2 image fill, -1 follow components
    // themeColor is the legacy storage name for the beautify-style background,
    // not an accent color. It is mutually exclusive with themeGradient.
    bool smartFill = true, themeColor = true, themeGradient = false;
    double themeOpacity = .65, themeAngle = 90;
    int foregroundContent = 0; // 0 original, 1 imported
    std::string foregroundImage;
    double iconX = .5, iconY = .5;
    int material = 0, componentTheme = 0; // plain/glass/acrylic; light/dark text
    double blurRadius = 24;
    bool edgeHighlight = false;
    double edgeWidth = 1, edgeStrength = .3;
    PanelGradient gradient;
    int effect = 0; // none, 3D, dynamic title
    int titleDirection = 0; // left, up
    bool autoTitleDirection = true;
    int titleWeight = 600;

    friend bool operator==(const LargeIconConfig&, const LargeIconConfig&) = default;
};

// One field inventory is shared by the on-disk codec and its validation. Unknown
// fields may be ignored, but an unknown config version is never silently rewritten.
template<class C, class F> void VisitLargeIconFields(C& c, F&& f)
{
#define LI_FIELD(name) f(#name, c.name)
    LI_FIELD(version); LI_FIELD(columns); LI_FIELD(rows);
    LI_FIELD(contentScale); LI_FIELD(fillScale); LI_FIELD(radius); LI_FIELD(radiusPercent); LI_FIELD(content); LI_FIELD(fit);
    LI_FIELD(focusX); LI_FIELD(focusY); LI_FIELD(image); LI_FIELD(cachedCover);
    LI_FIELD(autoColor); LI_FIELD(manualColor); LI_FIELD(colorMix);
    LI_FIELD(opacity); LI_FIELD(hoverOpacity); LI_FIELD(border);
    LI_FIELD(hoverOpacityLinked);
    LI_FIELD(borderColor); LI_FIELD(borderWidth); LI_FIELD(borderOpacity);
    LI_FIELD(shadow); LI_FIELD(shadowStrength); LI_FIELD(titleMode);
    LI_FIELD(titleSize); LI_FIELD(revealTitleSize); LI_FIELD(autoTitleColor); LI_FIELD(titleColor);
    LI_FIELD(hoverContent); LI_FIELD(hoverFrame); LI_FIELD(press);
    LI_FIELD(launch); LI_FIELD(coverHover); LI_FIELD(amplitude);
    LI_FIELD(delayMs); LI_FIELD(enterMs); LI_FIELD(exitMs);
    LI_FIELD(steamOrientation); LI_FIELD(localOnly);
    LI_FIELD(backgroundStyle); LI_FIELD(smartFill); LI_FIELD(themeColor); LI_FIELD(themeGradient);
    LI_FIELD(themeOpacity); LI_FIELD(themeAngle); LI_FIELD(foregroundContent); LI_FIELD(foregroundImage);
    LI_FIELD(iconX); LI_FIELD(iconY); LI_FIELD(material); LI_FIELD(componentTheme); LI_FIELD(blurRadius);
    LI_FIELD(edgeHighlight); LI_FIELD(edgeWidth); LI_FIELD(edgeStrength); LI_FIELD(gradient);
    LI_FIELD(effect); LI_FIELD(titleDirection); LI_FIELD(autoTitleDirection); LI_FIELD(titleWeight);
#undef LI_FIELD
}

inline bool IsManagedLargeIconImage(std::string_view name)
{
    // References are flat filenames relative to data/large-icons, never paths.
    return name.empty() || (name.size() <= 160 && name != "." && name != ".." &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        }));
}

inline bool ValidateLargeIconConfig(const LargeIconConfig& c)
{
    auto range = [](double value, double min, double max) {
        return std::isfinite(value) && value >= min && value <= max;
    };
    const bool style = c.backgroundStyle == -3 || c.backgroundStyle == -2 || c.backgroundStyle == -1 ||
        c.backgroundStyle == 0 || c.backgroundStyle == 1 || c.backgroundStyle == 6 || c.backgroundStyle == 7 ||
        c.backgroundStyle == 9 || c.backgroundStyle == 10 || c.backgroundStyle == 11;
    return (c.version == 1 || c.version == 2) && c.columns >= 1 && c.columns <= 1024 &&
        c.rows >= 1 && c.rows <= 1024 && range(c.contentScale, .1, 1) && range(c.fillScale, .25, 3) &&
        range(c.radius, 0, 512) && (c.radiusPercent == -1 || range(c.radiusPercent, 0, 100)) && c.content >= 0 && c.content <= 2 &&
        c.fit >= 0 && c.fit <= 1 && range(c.focusX, 0, 1) && range(c.focusY, 0, 1) &&
        IsManagedLargeIconImage(c.image) && IsManagedLargeIconImage(c.cachedCover) &&
        c.manualColor <= 0xffffff && c.borderColor <= 0xffffff && c.titleColor <= 0xffffff &&
        range(c.colorMix, 0, 1) && range(c.opacity, 0, 1) && range(c.hoverOpacity, 0, 1) &&
        range(c.borderWidth, 0, 16) && range(c.borderOpacity, 0, 1) &&
        range(c.shadowStrength, 0, 1) && c.titleMode >= 0 && c.titleMode <= 1 &&
        range(c.titleSize, 8, 72) && range(c.revealTitleSize, 8, 72) && c.hoverContent >= 0 && c.hoverContent <= 3 &&
        c.hoverFrame >= 0 && c.hoverFrame <= 3 && c.launch >= 0 && c.launch <= 2 &&
        c.coverHover >= 0 && c.coverHover <= 2 && range(c.amplitude, 0, 2) &&
        c.delayMs >= 0 && c.delayMs <= 2000 && c.enterMs >= 0 && c.enterMs <= 2000 &&
        c.exitMs >= 0 && c.exitMs <= 2000 && c.steamOrientation >= 0 && c.steamOrientation <= 2 &&
        style && !(c.themeColor && c.themeGradient) && range(c.themeOpacity, 0, 1) && range(c.themeAngle, 0, 360) &&
        c.foregroundContent >= 0 && c.foregroundContent <= 1 && IsManagedLargeIconImage(c.foregroundImage) &&
        range(c.iconX, 0, 1) && range(c.iconY, 0, 1) && c.material >= 0 && c.material <= 2 &&
        c.componentTheme >= 0 && c.componentTheme <= 1 && range(c.blurRadius, 4, 48) &&
        range(c.edgeWidth, .5, 4) && range(c.edgeStrength, 0, 1) && ValidatePanelGradient(c.gradient) &&
        c.effect >= 0 && c.effect <= 2 && c.titleDirection >= 0 && c.titleDirection <= 1 &&
        c.titleWeight >= 100 && c.titleWeight <= 900 && c.titleWeight % 100 == 0;
}

inline bool IsLargeIconFill(const LargeIconConfig& config) { return config.backgroundStyle == -2; }
inline int LargeIconActiveContent(const LargeIconConfig& config)
{ return IsLargeIconFill(config) ? config.content : config.foregroundContent; }
inline const std::string& LargeIconActiveImage(const LargeIconConfig& config)
{ return IsLargeIconFill(config) ? config.image : config.foregroundImage; }

inline bool DecodeLargeIconConfig(const JsonValue& value, LargeIconConfig& result)
{
    if (!value.IsObject() || !value.Find("version")) return false;
    LargeIconConfig c;
    if (value.Find("version")->IsNumber() && value.Find("version")->number == 1) c.fit = 0;
    bool valid = true;
    VisitLargeIconFields(c, [&](const char* name, auto& field) {
        const auto* v = value.Find(name);
        if (!v) return;
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, PanelGradient>)
        {
            if (!DecodePanelGradient(*v, field)) valid = false;
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            if (!v->IsString()) valid = false;
            else field = v->string;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            if (!v->IsBoolean()) valid = false;
            else field = v->boolean;
        }
        else
        {
            if (!v->IsNumber() || !std::isfinite(v->number)) { valid = false; return; }
            if constexpr (std::is_integral_v<T>)
                if (v->number < static_cast<double>(std::numeric_limits<T>::lowest()) ||
                    v->number > static_cast<double>(std::numeric_limits<T>::max()) ||
                    std::floor(v->number) != v->number) { valid = false; return; }
            field = static_cast<T>(v->number);
        }
    });
    // Earlier v2 layouts nested the gradient switch beneath themeColor.
    // Preserve their gradient while normalizing the now-exclusive modes.
    if (c.themeGradient) c.themeColor = false;
    if (!value.Find("autoTitleDirection")) c.autoTitleDirection = false;
    if (!valid || !ValidateLargeIconConfig(c)) return false;
    if (c.version == 1)
    {
        c.version = 2;
        c.backgroundStyle = c.content != 0 ? -2 : c.autoColor ? -3 : 9;
        c.themeOpacity = c.opacity;
        c.effect = c.titleMode == 1 ? 2 : 0;
        c.titleDirection = 0;
        // Explicit v1 images were full-frame content. The image reference stays
        // in the fill source; no copy is mistaken for a foreground replacement.
    }
    result = std::move(c);
    return true;
}

inline std::string EncodeLargeIconConfig(const LargeIconConfig& c)
{
    if (!ValidateLargeIconConfig(c)) return {};
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(17);
    out << '{';
    bool first = true;
    VisitLargeIconFields(c, [&](const char* name, const auto& field) {
        if (!first) out << ',';
        first = false;
        out << '"' << name << "\":";
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, PanelGradient>) out << EncodePanelGradient(field);
        else if constexpr (std::is_same_v<T, std::string>) out << '"' << field << '"';
        else if constexpr (std::is_same_v<T, bool>) out << (field ? "true" : "false");
        else out << field;
    });
    out << '}';
    return out.str();
}
}
