#include "native_tooltip_preferences.h"
#include "theme/personalization.h"

#include <algorithm>

namespace snowdesktop
{
std::atomic<int> nativeTooltipFontSize{kDefaultTooltipFontSize};
std::atomic<std::shared_ptr<const PersonalizationSettings>> nativeTooltipAppearance;
thread_local std::vector<HWND> nativeTooltipWindows;

int NormalizeTooltipFontSize(int value)
{
    return std::clamp(value, kMinimumTooltipFontSize, kMaximumTooltipFontSize);
}

float NativeTooltipFontSize()
{
    return static_cast<float>(nativeTooltipFontSize.load());
}

bool SetNativeTooltipPreferences(int fontSize, const PersonalizationSettings& appearance)
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

PersonalizationSettings NativeTooltipAppearance()
{
    const auto appearance = nativeTooltipAppearance.load();
    return appearance ? *appearance : PersonalizationSettings::DarkPreset();
}

float NativeTooltipCornerRadius(const PersonalizationSettings& appearance,
    float width, float height, float scale)
{
    return std::max(0.f, std::min({appearance.cornerRadius * scale, 8.f * scale, width * .25f, height * .25f}));
}
}
