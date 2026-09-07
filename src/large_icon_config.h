#pragma once

#include "json_value.h"

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
    int version = 1;
    int columns = 2, rows = 2;
    double contentScale = .60;
    double radius = 12;
    int content = 0; // 0 original, 1 imported static image, 2 Steam
    int fit = 0; // 0 contain, 1 cover
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

    friend bool operator==(const LargeIconConfig&, const LargeIconConfig&) = default;
};

// One field inventory is shared by the on-disk codec and its validation. Unknown
// fields may be ignored, but an unknown config version is never silently rewritten.
template<class C, class F> void VisitLargeIconFields(C& c, F&& f)
{
#define LI_FIELD(name) f(#name, c.name)
    LI_FIELD(version); LI_FIELD(columns); LI_FIELD(rows);
    LI_FIELD(contentScale); LI_FIELD(radius); LI_FIELD(content); LI_FIELD(fit);
    LI_FIELD(focusX); LI_FIELD(focusY); LI_FIELD(image); LI_FIELD(cachedCover);
    LI_FIELD(autoColor); LI_FIELD(manualColor); LI_FIELD(colorMix);
    LI_FIELD(opacity); LI_FIELD(hoverOpacity); LI_FIELD(border);
    LI_FIELD(hoverOpacityLinked);
    LI_FIELD(borderColor); LI_FIELD(borderWidth); LI_FIELD(borderOpacity);
    LI_FIELD(shadow); LI_FIELD(shadowStrength); LI_FIELD(titleMode);
    LI_FIELD(titleSize); LI_FIELD(autoTitleColor); LI_FIELD(titleColor);
    LI_FIELD(hoverContent); LI_FIELD(hoverFrame); LI_FIELD(press);
    LI_FIELD(launch); LI_FIELD(coverHover); LI_FIELD(amplitude);
    LI_FIELD(delayMs); LI_FIELD(enterMs); LI_FIELD(exitMs);
    LI_FIELD(steamOrientation); LI_FIELD(localOnly);
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
    return c.version == 1 && c.columns >= 1 && c.columns <= 1024 &&
        c.rows >= 1 && c.rows <= 1024 && range(c.contentScale, .1, 1) &&
        range(c.radius, 0, 512) && c.content >= 0 && c.content <= 2 &&
        c.fit >= 0 && c.fit <= 1 && range(c.focusX, 0, 1) && range(c.focusY, 0, 1) &&
        IsManagedLargeIconImage(c.image) && IsManagedLargeIconImage(c.cachedCover) &&
        c.manualColor <= 0xffffff && c.borderColor <= 0xffffff && c.titleColor <= 0xffffff &&
        range(c.colorMix, 0, 1) && range(c.opacity, 0, 1) && range(c.hoverOpacity, 0, 1) &&
        range(c.borderWidth, 0, 16) && range(c.borderOpacity, 0, 1) &&
        range(c.shadowStrength, 0, 1) && c.titleMode >= 0 && c.titleMode <= 1 &&
        range(c.titleSize, 8, 72) && c.hoverContent >= 0 && c.hoverContent <= 3 &&
        c.hoverFrame >= 0 && c.hoverFrame <= 3 && c.launch >= 0 && c.launch <= 2 &&
        c.coverHover >= 0 && c.coverHover <= 2 && range(c.amplitude, 0, 2) &&
        c.delayMs >= 0 && c.delayMs <= 2000 && c.enterMs >= 0 && c.enterMs <= 2000 &&
        c.exitMs >= 0 && c.exitMs <= 2000 && c.steamOrientation >= 0 && c.steamOrientation <= 2;
}

inline bool DecodeLargeIconConfig(const JsonValue& value, LargeIconConfig& result)
{
    if (!value.IsObject() || !value.Find("version")) return false;
    LargeIconConfig c;
    bool valid = true;
    VisitLargeIconFields(c, [&](const char* name, auto& field) {
        const auto* v = value.Find(name);
        if (!v) return;
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, std::string>)
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
    if (!valid || !ValidateLargeIconConfig(c)) return false;
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
        if constexpr (std::is_same_v<T, std::string>) out << '"' << field << '"';
        else if constexpr (std::is_same_v<T, bool>) out << (field ? "true" : "false");
        else out << field;
    });
    out << '}';
    return out.str();
}
}
