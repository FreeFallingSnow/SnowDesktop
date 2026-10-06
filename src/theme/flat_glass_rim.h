/*
 * Copyright (c) 2025-2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified for SnowDesktop in 2026: CPU opacity-mask adaptation of OpenHarmony
 * GraphicsEffect's SDF edge-light and frosted-glass fan-mask calculations.
 * Upstream: da8e11652a705ea2141c35de1a1fff501148740e.
 * Source paths, adaptation boundaries and license: third_party/graphics-effect/.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include "edge_light_settings.h"

namespace snowdesktop::flat_glass_rim
{
inline constexpr int kPanelOverdraw = 3;

inline float SmoothStep(float lower, float upper, float value)
{
    const float t = std::clamp((value - lower) / (upper - lower), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Adapted from FrostedGlassEffect::DiagonalFanMask. Direction, opening,
// feather and ambient light below are SnowDesktop tuning, not Honor values.
// Broad constant sectors keep the corners lit; the transition is localized
// around the two weak sectors instead of tapering along the entire contour.
class Evaluator
{
public:
    explicit Evaluator(EdgeLightSettings value = {}) : settings(NormalizeEdgeLight(value))
    {
        constexpr float radians = 3.14159265358979323846f / 180.f;
        directionX = std::cos(settings.direction * radians);
        directionY = std::sin(settings.direction * radians);
        opening = settings.spread * .5f * radians;
        feather = settings.feather * radians;
    }
    EdgeLightSettings settings;
    float directionX, directionY, opening, feather;
    float Lighting(float x, float y) const
    {
    x = std::clamp(x, 0.0f, 1.0f) - 0.5f;
    y = std::clamp(y, 0.0f, 1.0f) - 0.5f;
    const float length = std::hypot(x, y);
    if (length <= 0.00001f) return settings.ambient;
    const float alignment = std::clamp(
        (x * directionX + y * directionY) /
            length, -1.0f, 1.0f);
    const float facingAngle = std::acos(alignment);
    const float opposingAngle = std::acos(-alignment);
    const float facing = 1.0f - SmoothStep(
        opening, opening + feather, facingAngle);
    const float opposing = 1.0f - SmoothStep(
        opening, opening + feather, opposingAngle);
    return std::clamp(settings.ambient + settings.primary * facing + settings.opposite * opposing, 0.f, 1.f);
    }

// Adapted from SDFEdgeLightEffect::EdgeLightFakeBloom. Minimum thickness,
// nonlinear width mapping and independent inner/outer bloom are retained.
// Dimensions and bloom strength are tuned for SnowDesktop's small plates.
float BorderWidth(float depth, float lighting) const
{
    return depth * (settings.minimumWidth + settings.widthVariation * SmoothStep(0.0f, 1.0f, lighting));
}

float Intensity(float distance, float depth, float lighting) const
{
    if (depth <= 0.0f) return 0.0f;
    const float thickness = BorderWidth(depth, lighting);
    const float coreWidth = distance >= 0.0f
        ? thickness : std::min(thickness, static_cast<float>(kPanelOverdraw));
    const float core = 1.0f - SmoothStep(0.0f, coreWidth, std::abs(distance));
    const float bloomWidth = distance >= 0.0f
        ? depth * settings.innerGlow
        : std::min(depth * settings.outerGlow, static_cast<float>(kPanelOverdraw));
    const float normalizedDistance = bloomWidth > 0.f ? std::abs(distance) / bloomWidth : 1.f;
    const float falloff = normalizedDistance < 1.f ? (1.f - normalizedDistance) /
        std::pow(1.f + normalizedDistance, settings.glowFalloff) : 0.f;
    const float bloomGate = SmoothStep(settings.glowThreshold, 1.0f, lighting);
    return std::clamp(lighting * (core + settings.glowStrength * bloomGate * falloff), 0.0f, 1.0f);
}

// A restrained inner seam belongs to the glass body independently of the
// directional reflection. Use the frosted shader's exponential decay rather
// than a second Gaussian crest which makes the cross-section look rounded.
float Occlusion(float distance, float depth, float lighting) const
{
    if (depth <= 0.0f || distance <= 0.0f) return 0.0f;
    const float thickness = BorderWidth(depth, lighting);
    const float entry = SmoothStep(thickness * 0.6f, thickness, distance);
    return entry * std::exp(-settings.shadowFalloff * std::max(distance - thickness, 0.0f) / depth);
}
// Outside this band every possible mask sample rounds to zero in A8. Keeping
// the support finite also bounds icon and animated-panel work by perimeter.
float InnerSupport(float depth, bool occlusion) const
{
    const float core = depth * (settings.minimumWidth + settings.widthVariation);
    return occlusion ? core + depth * std::log(510.f) / settings.shadowFalloff
        : std::max(core, depth * settings.innerGlow);
}
};
inline float Lighting(float x, float y) { return Evaluator{}.Lighting(x, y); }
inline float BorderWidth(float depth, float lighting) { return Evaluator{}.BorderWidth(depth, lighting); }
inline float Intensity(float distance, float depth, float lighting) { return Evaluator{}.Intensity(distance, depth, lighting); }
inline float Occlusion(float distance, float depth, float lighting) { return Evaluator{}.Occlusion(distance, depth, lighting); }
}
