#pragma once

namespace snowdesktop
{
struct IconShadowSettings
{
    bool fan = true;
    bool dock = false;
    bool exposedWidget = false;
    bool desktop = false;
    bool popup = false;
    bool quickNavigation = false;

    bool operator==(const IconShadowSettings&) const = default;
};
} // namespace snowdesktop
