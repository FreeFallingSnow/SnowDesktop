#pragma once
#include <algorithm>
#include <cmath>

namespace snowdesktop
{
// Shared material values. A theme selects these values; it never selects a
// different renderer. Width/strength remain the existing surface properties.
struct EdgeLightSettings
{
    float direction = 60.f;
    float spread = 130.f;
    float feather = 20.f;
    float ambient = .56f;
    float primary = .34f;
    float opposite = .30f;
    float minimumWidth = .65f;
    float widthVariation = .45f;
    float innerGlow = 2.f;
    float outerGlow = 1.2f;
    float glowStrength = .30f;
    float glowFalloff = 2.f;
    float glowThreshold = .10f;
    float shadowStrength = .12f;
    float shadowFalloff = 4.6f;
    bool operator==(const EdgeLightSettings&) const = default;
};

template<class Visit> void VisitEdgeLightFields(Visit visit)
{
    visit("direction", &EdgeLightSettings::direction, 0.f, 360.f);
    visit("spread", &EdgeLightSettings::spread, 0.f, 180.f);
    visit("feather", &EdgeLightSettings::feather, .5f, 90.f);
    visit("ambient", &EdgeLightSettings::ambient, 0.f, 1.f);
    visit("primary", &EdgeLightSettings::primary, 0.f, 1.f);
    visit("opposite", &EdgeLightSettings::opposite, 0.f, 1.f);
    visit("minimumWidth", &EdgeLightSettings::minimumWidth, .1f, 1.f);
    visit("widthVariation", &EdgeLightSettings::widthVariation, 0.f, 1.f);
    visit("innerGlow", &EdgeLightSettings::innerGlow, 0.f, 4.f);
    visit("outerGlow", &EdgeLightSettings::outerGlow, 0.f, 3.f);
    visit("glowStrength", &EdgeLightSettings::glowStrength, 0.f, 1.f);
    visit("glowFalloff", &EdgeLightSettings::glowFalloff, .5f, 8.f);
    visit("glowThreshold", &EdgeLightSettings::glowThreshold, 0.f, .99f);
    visit("shadowStrength", &EdgeLightSettings::shadowStrength, 0.f, 1.f);
    visit("shadowFalloff", &EdgeLightSettings::shadowFalloff, 1.f, 12.f);
}

inline EdgeLightSettings NormalizeEdgeLight(EdgeLightSettings value)
{
    const EdgeLightSettings defaults;
    VisitEdgeLightFields([&](auto, auto field, float minimum, float maximum) {
        value.*field = std::isfinite(value.*field)
            ? std::clamp(value.*field, minimum, maximum) : defaults.*field;
    });
    return value;
}
inline bool ValidateEdgeLight(const EdgeLightSettings& value)
{
    bool valid = true;
    VisitEdgeLightFields([&](auto, auto field, float minimum, float maximum) {
        valid = valid && std::isfinite(value.*field) && value.*field >= minimum && value.*field <= maximum;
    });
    return valid;
}
}
