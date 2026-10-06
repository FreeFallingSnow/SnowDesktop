#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::font_weight_rules
{
inline constexpr int kDefaultWeight = 520;
inline constexpr int kMinimumWeight = 100;
inline constexpr int kMaximumWeight = 950;

// Percentages are presentation values. Keep the persisted DirectWrite weight
// unchanged so legacy layouts retain their exact setting on every reload.
inline constexpr double ToPercent(int weight) noexcept
{
    return weight * 100.0 / kDefaultWeight;
}

inline int FromPercent(double percent) noexcept
{
    if (!std::isfinite(percent)) return kDefaultWeight;
    const double bounded = std::clamp(percent,
        ToPercent(kMinimumWeight), ToPercent(kMaximumWeight));
    return static_cast<int>(std::lround(bounded * kDefaultWeight / 100.0));
}

inline constexpr int RenderedWeight(int storedWeight, bool darkForeground) noexcept
{
    // DirectWrite accepts integer weights from 1 to 999, including 80 for the
    // darkest rendering of the minimum stored weight. Do not change the setting.
    const int weight = std::clamp(storedWeight, kMinimumWeight, kMaximumWeight);
    return darkForeground ? (weight * 4 + 2) / 5 : weight;
}
} // namespace snowdesktop::font_weight_rules
