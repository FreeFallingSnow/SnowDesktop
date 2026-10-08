#pragma once
#include "theme/personalization.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace snowdesktop
{
inline constexpr int kDefaultTooltipFontSize = 12;
inline constexpr int kMinimumTooltipFontSize = 10;
inline constexpr int kMaximumTooltipFontSize = 24;
inline int NormalizeTooltipFontSize(int value)
{
    return std::clamp(value, kMinimumTooltipFontSize, kMaximumTooltipFontSize);
}

// Process preferences, like app_fonts::current. Shared by popup windows and
// inline desktop/widget renderers; no component API or saved theme field.
inline std::atomic<int> nativeTooltipFontSize{kDefaultTooltipFontSize};
inline std::atomic<std::shared_ptr<const PersonalizationSettings>> nativeTooltipAppearance;
inline constexpr UINT kNativeTooltipPreferencesChanged = WM_APP + 0x69c;
inline thread_local std::vector<HWND> nativeTooltipWindows;
inline float NativeTooltipFontSize()
{
    return static_cast<float>(nativeTooltipFontSize.load());
}
inline bool SetNativeTooltipPreferences(int fontSize, const PersonalizationSettings& appearance)
{
    const int size = NormalizeTooltipFontSize(fontSize);
    const bool sizeChanged = nativeTooltipFontSize.exchange(size) != size;
    const auto previous = nativeTooltipAppearance.load();
    const bool appearanceChanged = !previous || *previous != appearance;
    if (appearanceChanged)
        nativeTooltipAppearance.store(std::make_shared<const PersonalizationSettings>(appearance));
    if (sizeChanged || appearanceChanged)
        for (const auto window : nativeTooltipWindows)
            PostMessageW(window, kNativeTooltipPreferencesChanged, 0, 0);
    return sizeChanged || appearanceChanged;
}
inline PersonalizationSettings NativeTooltipAppearance()
{
    const auto appearance = nativeTooltipAppearance.load();
    return appearance ? *appearance : PersonalizationSettings::DarkPreset();
}
inline float NativeTooltipCornerRadius(const PersonalizationSettings& appearance,
    float width, float height, float scale = 1.f)
{
    return std::max(0.f, std::min({appearance.cornerRadius * scale, 8.f * scale, width * .25f, height * .25f}));
}
}
