#pragma once
#include "edge_light_settings.h"

namespace snowdesktop
{
enum class MaterialEdgePreset { GlassDark, GlassLight, GlassTransparent, AcrylicDark, AcrylicLight };
struct MaterialEdgeProfile
{
    float width = 1.2f;
    float opacity = .42f;
    EdgeLightSettings light;
};

// The web preview specifies full core thickness. The native evaluator keeps
// its existing half-band depth convention: depth = web width / 2, glow ratios
// = web ratios * 2, and seam falloff = web falloff / 2. Saved custom values and
// component API semantics therefore remain unchanged.
inline MaterialEdgeProfile MaterialEdges(MaterialEdgePreset preset)
{
    MaterialEdgeProfile p;
    auto& e = p.light;
    e.direction = 60; e.spread = 100; e.feather = 35;
    e.minimumWidth = .65f; e.widthVariation = .35f;
    e.glowThreshold = .10f;
    switch (preset)
    {
    case MaterialEdgePreset::GlassDark:
        p.width = 1.25f; p.opacity = .34f;
        e.ambient = .58f; e.primary = .30f; e.opposite = .26f;
        e.innerGlow = 1.f; e.outerGlow = .4f; e.glowStrength = .14f; e.glowFalloff = 2.8f;
        e.shadowStrength = .14f; e.shadowFalloff = 3.95f;
        break;
    case MaterialEdgePreset::GlassLight:
        p.opacity = .38f;
        e.ambient = .64f; e.primary = .24f; e.opposite = .20f;
        e.innerGlow = 1.2f; e.outerGlow = .6f; e.glowStrength = .16f; e.glowFalloff = 2.5f;
        e.shadowStrength = .20f; e.shadowFalloff = 4.65f;
        break;
    case MaterialEdgePreset::GlassTransparent:
        e.ambient = .62f; e.primary = .28f; e.opposite = .24f;
        e.innerGlow = 1.2f; e.outerGlow = .6f; e.glowStrength = .20f; e.glowFalloff = 2.2f;
        e.shadowStrength = .10f; e.shadowFalloff = 4.f;
        break;
    case MaterialEdgePreset::AcrylicDark:
        p.opacity = .30f;
        e.ambient = .60f; e.primary = .26f; e.opposite = .22f;
        e.innerGlow = 1.4f; e.outerGlow = .6f; e.glowStrength = .10f; e.glowFalloff = 3.f;
        e.shadowStrength = .12f; e.shadowFalloff = 4.15f;
        break;
    case MaterialEdgePreset::AcrylicLight:
        p.opacity = .38f;
        e.ambient = .62f; e.primary = .26f; e.opposite = .22f;
        e.innerGlow = 1.4f; e.outerGlow = .6f; e.glowStrength = .12f; e.glowFalloff = 2.8f;
        e.shadowStrength = .28f; e.shadowFalloff = 4.4f;
        break;
    }
    return p;
}
}
