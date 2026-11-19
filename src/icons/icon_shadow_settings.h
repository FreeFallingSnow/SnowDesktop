#pragma once

#include <algorithm>

namespace snowdesktop
{
inline constexpr int kDefaultIconShadowStrength = 65;

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
        fanStrength = std::clamp(fanStrength, 0, 100);
        dockStrength = std::clamp(dockStrength, 0, 100);
        exposedWidgetStrength = std::clamp(exposedWidgetStrength, 0, 100);
        desktopStrength = std::clamp(desktopStrength, 0, 100);
        popupStrength = std::clamp(popupStrength, 0, 100);
        quickNavigationStrength = std::clamp(quickNavigationStrength, 0, 100);
        subjectStrength = std::clamp(subjectStrength, 0, 100);
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
