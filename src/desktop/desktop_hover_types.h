#pragma once

namespace snowdesktop::desktop_hover_rules
{
// Keep application declarations independent of the frequently changed policies.
enum class ReconcileMode
{
    DeactivateOnly,
    AllowImmediateActivation,
    AllowActivationAfterForegroundSettle,
};

struct RetainedDockLeaveTracking
{
    int screenX = 0, screenY = 0;
    bool pointerKnown = false;
    bool contentRearmed = false, desktopRearmed = false;
};
}
