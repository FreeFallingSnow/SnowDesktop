#pragma once

#include "native_tooltip_settings.h"
#include "theme/personalization_types.h"

#include <windows.h>
#include <atomic>
#include <memory>
#include <vector>

namespace snowdesktop
{
extern std::atomic<int> nativeTooltipFontSize;
extern std::atomic<std::shared_ptr<const PersonalizationSettings>> nativeTooltipAppearance;
inline constexpr UINT kNativeTooltipPreferencesChanged = WM_APP + 0x69c;
extern thread_local std::vector<HWND> nativeTooltipWindows;

float NativeTooltipFontSize();

bool SetNativeTooltipPreferences(int fontSize, const PersonalizationSettings& appearance);

PersonalizationSettings NativeTooltipAppearance();

float NativeTooltipCornerRadius(const PersonalizationSettings& appearance,
    float width, float height, float scale = 1.f);
}
