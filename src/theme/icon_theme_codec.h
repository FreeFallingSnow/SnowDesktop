#pragma once
#include "icons/icon_beautify.h"
#include "common/json_value.h"
#include "edge_light_codec.h"
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace snowdesktop::themes
{
// Only visual values travel with an icon theme. Preset identity belongs to the
// local selector; imported themes always use the custom preset.
template<class Visit> void VisitIconThemeNumbers(Visit visit)
{
#define FIELD(name, low, high) visit(#name, &IconBeautifySettings::name, low, high)
    FIELD(backgroundOpacity, 0.f, 1.f); FIELD(glassBlurRadius, 4.f, 48.f);
    FIELD(edgeHighlightWidth, .5f, 4.f); FIELD(edgeHighlightStrength, 0.f, 1.f);
    FIELD(backgroundStartR, 0.f, 1.f); FIELD(backgroundStartG, 0.f, 1.f); FIELD(backgroundStartB, 0.f, 1.f);
    FIELD(backgroundEndR, 0.f, 1.f); FIELD(backgroundEndG, 0.f, 1.f); FIELD(backgroundEndB, 0.f, 1.f);
    FIELD(contentScale, .5f, .9f); FIELD(textureHighlightStrength, 0.f, 1.f);
    FIELD(textureHighlightSize, .1f, 1.f); FIELD(textureHighlightAngle, -1.f, 1.f);
    FIELD(textureShadeStrength, 0.f, 1.f); FIELD(textureEdgeHighlight, 0.f, 1.f);
    FIELD(filterStrength, 0.f, 1.f); FIELD(filterTintR, 0.f, 1.f); FIELD(filterTintG, 0.f, 1.f); FIELD(filterTintB, 0.f, 1.f);
    FIELD(outlineWidth, 0.f, 4.f); FIELD(outlineOpacity, 0.f, 1.f);
    FIELD(outlineR, 0.f, 1.f); FIELD(outlineG, 0.f, 1.f); FIELD(outlineB, 0.f, 1.f); FIELD(shadowStrength, 0.f, 1.f);
#undef FIELD
}
template<class Visit> void VisitIconThemeFlags(Visit visit)
{
#define FIELD(name) visit(#name, &IconBeautifySettings::name)
    FIELD(enabled); FIELD(glassEnabled); FIELD(edgeHighlightEnabled); FIELD(gradientEnabled);
    FIELD(filterEnabled); FIELD(outlineEnabled);
#undef FIELD
}
inline bool ValidIconTheme(const IconBeautifySettings& value)
{
    const auto shape = static_cast<int>(value.shape);
    bool valid = value.mode >= 0 && value.mode <= 1 && value.gradientDirection >= 0 && value.gradientDirection <= 3 &&
        (shape == 0 || shape == 1 || shape == 2 || shape == 3 || shape == 10) && ValidateEdgeLight(value.edgeLight);
    VisitIconThemeNumbers([&](auto, auto field, float low, float high) {
        valid = valid && std::isfinite(value.*field) && value.*field >= low && value.*field <= high;
    });
    return valid;
}
inline bool SameIconTheme(const IconBeautifySettings& a, const IconBeautifySettings& b)
{
    bool same = a.mode == b.mode && a.gradientDirection == b.gradientDirection && a.shape == b.shape && a.edgeLight == b.edgeLight;
    VisitIconThemeNumbers([&](auto, auto field, float, float) { same = same && a.*field == b.*field; });
    VisitIconThemeFlags([&](auto, auto field) { same = same && a.*field == b.*field; });
    return same;
}
inline std::string EncodeIconTheme(const IconBeautifySettings& value)
{
    if (!ValidIconTheme(value)) return {};
    std::ostringstream out; out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\"mode\":" << value.mode << ",\"shape\":" << static_cast<int>(value.shape)
        << ",\"gradientDirection\":" << value.gradientDirection << ",\"edgeLight\":" << EncodeEdgeLight(value.edgeLight);
    VisitIconThemeNumbers([&](auto name, auto field, float, float) { out << ",\"" << name << "\":" << value.*field; });
    VisitIconThemeFlags([&](auto name, auto field) { out << ",\"" << name << "\":" << (value.*field ? "true" : "false"); });
    return out.str() + '}';
}
inline bool DecodeIconTheme(const JsonValue& json, IconBeautifySettings& value)
{
    if (!json.IsObject()) return false;
    IconBeautifySettings out; out.preset = IconBeautifyPreset::Custom;
    std::set<std::string> keys{"mode", "shape", "gradientDirection", "edgeLight"};
    bool valid = true;
    const auto integer = [&](const char* name, int& into) {
        const auto* entry = json.Find(name);
        if (!entry || !entry->IsNumber() || !std::isfinite(entry->number) ||
            std::floor(entry->number) != entry->number || entry->number < 0 || entry->number > 10) valid = false;
        else into = static_cast<int>(entry->number);
    };
    int shape = 0; integer("mode", out.mode); integer("shape", shape); integer("gradientDirection", out.gradientDirection);
    out.shape = static_cast<IconBeautifyShape>(shape);
    const auto* light = json.Find("edgeLight");
    if (!light || !DecodeEdgeLight(*light, out.edgeLight)) return false;
    std::set<std::string> lightKeys;
    VisitEdgeLightFields([&](auto name, auto, float, float) { lightKeys.insert(name); if (!light->Find(name)) valid = false; });
    for (const auto& [key, entry] : light->object) { (void)entry; if (!lightKeys.contains(key)) valid = false; }
    VisitIconThemeNumbers([&](auto name, auto field, float low, float high) {
        keys.insert(name); const auto* entry = json.Find(name);
        if (!entry || !entry->IsNumber() || !std::isfinite(entry->number)) { valid = false; return; }
        const auto decoded = static_cast<float>(entry->number);
        if (!std::isfinite(decoded) || decoded < low || decoded > high) valid = false;
        else out.*field = decoded;
    });
    VisitIconThemeFlags([&](auto name, auto field) {
        keys.insert(name); const auto* entry = json.Find(name);
        if (!entry || !entry->IsBoolean()) valid = false; else out.*field = entry->boolean;
    });
    for (const auto& [key, entry] : json.object) { (void)entry; if (!keys.contains(key)) valid = false; }
    if (!valid || !ValidIconTheme(out)) return false;
    value = out; return true;
}
}
