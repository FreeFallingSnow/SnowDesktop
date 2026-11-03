#pragma once

namespace snowdesktop::dock_attention_dot
{
inline constexpr float kDefaultSizeScale = 1.0f;
inline constexpr float kMinimumSizeScale = 0.5f;
inline constexpr float kMaximumSizeScale = 2.0f;
inline constexpr float kDefaultOffset = 0.0f;
inline constexpr float kMinimumOffset = -0.25f;
inline constexpr float kMaximumOffset = 0.25f;

float NormalizeSizeScale(float value) noexcept;

float NormalizeOffset(float value) noexcept;

struct Geometry
{
    float radius = 0.0f;
    float cornerInset = 0.0f;
};

Geometry Resolve(float iconSize, float sizeScale, float offset) noexcept;
} // namespace snowdesktop::dock_attention_dot
