#pragma once

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

}
