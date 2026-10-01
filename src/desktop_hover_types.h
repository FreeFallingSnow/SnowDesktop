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
}
