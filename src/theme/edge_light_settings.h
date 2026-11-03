#pragma once
#include "edge_light_types.h"

namespace snowdesktop
{
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

EdgeLightSettings NormalizeEdgeLight(EdgeLightSettings value);
bool ValidateEdgeLight(const EdgeLightSettings& value);
}
