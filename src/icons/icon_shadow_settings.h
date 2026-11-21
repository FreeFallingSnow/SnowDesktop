#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop
{
inline constexpr int kMinIconShadowStrength = 20;
inline constexpr int kDefaultIconShadowStrength = 100;
inline constexpr int kMaxIconShadowStrength = 200;
inline constexpr float kOriginalIconShadowOpacity = 0.65f;
inline constexpr float kIconShadowBlur = 2.5f;
inline constexpr float kIconShadowOffset = 3.0f;

inline float IconShadowOpacity(int strength) noexcept
{
    if (strength == kDefaultIconShadowStrength) return kOriginalIconShadowOpacity;
    if (strength <= 0) return 0.0f;
    // Relative strength compounds the original shadow without clipping at 100% alpha.
    return 1.0f - std::pow(1.0f - kOriginalIconShadowOpacity, strength / 100.0f);
}

inline int RelativeIconShadowStrength(int oldOpacity) noexcept
{
    if (oldOpacity >= 100) return kMaxIconShadowStrength;
    if (oldOpacity <= 0) return kMinIconShadowStrength;
    return static_cast<int>(std::lround(100.0f * std::log1p(-oldOpacity / 100.0f) /
        std::log1p(-kOriginalIconShadowOpacity)));
}

struct IconShadowSettings
{
    bool fan = true;
    bool dock = false;
    bool exposedWidget = false;
    bool desktop = false;
    bool popup = false;
    bool quickNavigation = false;
    bool subject = false;

    int fanStrength = kDefaultIconShadowStrength;
    int dockStrength = kDefaultIconShadowStrength;
    int exposedWidgetStrength = kDefaultIconShadowStrength;
    int desktopStrength = kDefaultIconShadowStrength;
    int popupStrength = kDefaultIconShadowStrength;
    int quickNavigationStrength = kDefaultIconShadowStrength;
    int subjectStrength = kDefaultIconShadowStrength;

    void Normalize() noexcept
    {
        fanStrength = std::clamp(fanStrength, kMinIconShadowStrength, kMaxIconShadowStrength);
        dockStrength = std::clamp(dockStrength, kMinIconShadowStrength, kMaxIconShadowStrength);
        exposedWidgetStrength = std::clamp(exposedWidgetStrength, kMinIconShadowStrength, kMaxIconShadowStrength);
        desktopStrength = std::clamp(desktopStrength, kMinIconShadowStrength, kMaxIconShadowStrength);
        popupStrength = std::clamp(popupStrength, kMinIconShadowStrength, kMaxIconShadowStrength);
        quickNavigationStrength = std::clamp(quickNavigationStrength, kMinIconShadowStrength, kMaxIconShadowStrength);
        subjectStrength = std::clamp(subjectStrength, kMinIconShadowStrength, kMaxIconShadowStrength);
    }

    int DockStrength() const noexcept { return dock ? dockStrength : 0; }
    int WidgetStrength() const noexcept { return exposedWidget ? exposedWidgetStrength : 0; }
    int PopupStrength() const noexcept { return popup ? popupStrength : 0; }
    int NavigationStrength() const noexcept { return quickNavigation ? quickNavigationStrength : 0; }
    int SubjectStrength(int locationStrength) const noexcept
    {
        return subject && subjectStrength > locationStrength ? subjectStrength : 0;
    }

    bool operator==(const IconShadowSettings&) const = default;
};
} // namespace snowdesktop
