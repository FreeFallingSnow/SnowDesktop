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
inline float Lighting(float x, float y)
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kReflectionRotation = kPi / 3.0f;
    constexpr float kHalfOpening = 75.0f * kPi / 180.0f;
    constexpr float kFeather = 15.0f * kPi / 180.0f;
    x = std::clamp(x, 0.0f, 1.0f) - 0.5f;
    y = std::clamp(y, 0.0f, 1.0f) - 0.5f;
    const float length = std::hypot(x, y);
    if (length <= 0.00001f) return 0.52f;
    const float alignment = std::clamp(
        (x * std::cos(kReflectionRotation) + y * std::sin(kReflectionRotation)) /
            length, -1.0f, 1.0f);
    const float facingAngle = std::acos(alignment);
    const float opposingAngle = std::acos(-alignment);
    const float facing = 1.0f - SmoothStep(
        kHalfOpening, kHalfOpening + kFeather, facingAngle);
    const float opposing = 1.0f - SmoothStep(
        kHalfOpening, kHalfOpening + kFeather, opposingAngle);
    return 0.52f + 0.38f * facing + 0.30f * opposing;
}

// Adapted from SDFEdgeLightEffect::EdgeLightFakeBloom. Minimum thickness,
// nonlinear width mapping and independent inner/outer bloom are retained.
// Dimensions and bloom strength are tuned for SnowDesktop's small plates.
inline float BorderWidth(float depth, float lighting)
{
    return depth * (0.65f + 0.45f * SmoothStep(0.0f, 1.0f, lighting));
}

inline float Intensity(float distance, float depth, float lighting)
{
    if (depth <= 0.0f) return 0.0f;
    const float thickness = BorderWidth(depth, lighting);
    const float coreWidth = distance >= 0.0f
        ? thickness : std::min(thickness, static_cast<float>(kPanelOverdraw));
    const float core = 1.0f - SmoothStep(0.0f, coreWidth, std::abs(distance));
    const float bloomWidth = distance >= 0.0f
        ? depth * 2.0f
        : std::min(depth * 1.2f, static_cast<float>(kPanelOverdraw));
    const float normalizedDistance = std::abs(distance) / bloomWidth;
    const float falloff = std::max((1.0f - normalizedDistance) /
        ((1.0f + normalizedDistance) * (1.0f + normalizedDistance)), 0.0f);
    const float bloomGate = SmoothStep(0.1f, 1.0f, lighting);
    return std::clamp(lighting * (core + 0.30f * bloomGate * falloff), 0.0f, 1.0f);
}

// A restrained inner seam belongs to the glass body independently of the
// directional reflection. Use the frosted shader's exponential decay rather
// than a second Gaussian crest which makes the cross-section look rounded.
inline float Occlusion(float distance, float depth, float lighting)
{
    if (depth <= 0.0f || distance <= 0.0f) return 0.0f;
    const float thickness = BorderWidth(depth, lighting);
    const float entry = SmoothStep(thickness * 0.6f, thickness, distance);
    return entry * std::exp(-4.62f * std::max(distance - thickness, 0.0f) / depth);
}
}
