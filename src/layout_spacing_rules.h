#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace snowdesktop::layout_spacing_rules
{

inline constexpr float kMinimumScale = 0.50f;
inline constexpr float kMaximumScale = 2.00f;

inline float ClampScale(float value)
{
    if (!std::isfinite(value)) return 1.0f;
    return std::clamp(value, kMinimumScale, kMaximumScale);
}

inline float ResolveStoredScale(
    const std::optional<float>& iconSpacing,
    const std::optional<float>& legacyComponentSpacing,
    float fallback = 1.0f)
{
    // iconSpacing is canonical whenever it exists. componentSpacing is kept
    // only as a read-only migration source for older layout files.
    if (iconSpacing) return ClampScale(*iconSpacing);
    if (legacyComponentSpacing)
        return ClampScale(*legacyComponentSpacing);
    return ClampScale(fallback);
}

} // namespace snowdesktop::layout_spacing_rules
