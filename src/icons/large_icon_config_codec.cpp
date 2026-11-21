#include "large_icon_config_codec.h"

#include "common/json_value.h"
#include "icon_library.h"
#include "large_icon_config.h"
#include "theme/edge_light_codec.h"
#include "theme/panel_gradient.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>
#include <utility>

namespace snowdesktop
{
bool IsManagedLargeIconImage(std::string_view name)
{
    // References are flat filenames relative to data/large-icons, never paths.
    return name.empty() || (name.size() <= 160 && name != "." && name != ".." &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        }));
}

bool ValidateLargeIconConfig(const LargeIconConfig& c)
{
    auto range = [](double value, double min, double max) {
        return std::isfinite(value) && value >= min && value <= max;
    };
    const bool style = c.backgroundStyle == -5 || c.backgroundStyle == -4 || c.backgroundStyle == -3 || c.backgroundStyle == -2 || c.backgroundStyle == -1 ||
        c.backgroundStyle == 0 || c.backgroundStyle == 1 || c.backgroundStyle == 6 || c.backgroundStyle == 7 ||
        c.backgroundStyle == 9 || c.backgroundStyle == 10 || c.backgroundStyle == 11 || c.backgroundStyle == 13;
    return (c.version == 1 || c.version == 2 || c.version == 3) &&
        (c.imageAssetId.empty() || (c.version >= 3 && IsIconAssetId(c.imageAssetId))) &&
        (c.foregroundImageAssetId.empty() || (c.version >= 3 && IsIconAssetId(c.foregroundImageAssetId))) &&
        c.columns >= 1 && c.columns <= 1024 &&
        c.rows >= 1 && c.rows <= 1024 && c.shape >= 0 && c.shape <= 5 && c.flagDirection >= 0 && c.flagDirection <= 3 &&
        range(c.contentScale, .1, 1) && range(c.fillScale, .25, 3) &&
        range(c.radius, 0, 512) && (c.radiusPercent == -1 || range(c.radiusPercent, 0, 100)) && c.content >= 0 && c.content <= kLargeIconEpicCover &&
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
        c.defaultBackground >= 0 && c.defaultBackground <= 2 && c.defaultSolidColor <= 0xffffff &&
        range(c.defaultSolidOpacity, 0, 1) && ValidatePanelGradient(c.defaultGradient) &&
        style && !(c.themeColor && c.themeGradient) && range(c.themeOpacity, 0, 1) && range(c.themeAngle, 0, 360) &&
        c.foregroundContent >= 0 && c.foregroundContent <= 1 && IsManagedLargeIconImage(c.foregroundImage) &&
        range(c.iconX, 0, 1) && range(c.iconY, 0, 1) && c.material >= 0 && c.material <= 2 &&
        c.componentTheme >= 0 && c.componentTheme <= 1 && range(c.blurRadius, 4, 48) &&
        ValidateEdgeLight(c.edgeLight) && range(c.edgeWidth, .5, 4) && range(c.edgeStrength, 0, 1) && ValidatePanelGradient(c.gradient) && range(c.gradientOpacity, 0, 1) &&
        c.effect >= 0 && c.effect <= 5 && range(c.zoomAmount, 0, .10) &&
        range(c.glowStrength, 0, 1) && range(c.shineStrength, 0, 1) &&
        c.shineDurationMs >= 150 && c.shineDurationMs <= 1500 && c.titleDirection >= 0 && c.titleDirection <= 1 &&
        c.titleWeight >= 100 && c.titleWeight <= 900 && c.titleWeight % 100 == 0;
}

bool DecodeLargeIconConfig(const JsonValue& value, LargeIconConfig& result)
{
    if (!value.IsObject() || !value.Find("version")) return false;
    LargeIconConfig c;
    // Missing fields retain the defaults of the old on-disk format, not new-creation defaults.
    c.columns = c.rows = 2;
    c.backgroundStyle = -3;
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
        else if constexpr (std::is_same_v<T, EdgeLightSettings>)
        {
            if (!DecodeEdgeLight(*v, field)) valid = false;
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
    if (!value.Find("defaultBackground"))
    {
        c.defaultBackground = c.themeGradient ? 1 : 0;
        if (!c.themeGradient && !c.themeColor) { c.smartFill = false; c.themeOpacity = 1; }
    }
    c.themeGradient = c.defaultBackground == 1;
    c.themeColor = c.defaultBackground == 0;
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
    // The retired neutral preset becomes the explicit light component theme.
    // Decode only: existing custom, plate and fill backgrounds remain intact.
    if (c.backgroundStyle == -5) c.backgroundStyle = 1;
    result = std::move(c);
    return true;
}

std::string EncodeLargeIconConfig(const LargeIconConfig& c)
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
        else if constexpr (std::is_same_v<T, EdgeLightSettings>) out << EncodeEdgeLight(field);
        else if constexpr (std::is_same_v<T, std::string>) out << '"' << field << '"';
        else if constexpr (std::is_same_v<T, bool>) out << (field ? "true" : "false");
        else out << field;
    });
    out << '}';
    return out.str();
}
}
