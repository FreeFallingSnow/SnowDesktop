#pragma once

#include "settings_fwd.h"

namespace snowdesktop::settings_update_rules
{

bool IsGeneralShortcutOnlyCommit(
    const GeneralSettings& before,
    const GeneralSettings& after) noexcept;
bool IsGeneralLoggingOnlyCommit(const GeneralSettings& before,
    const GeneralSettings& after) noexcept;
bool IsNavigationShortcutOnlyCommit(
    const NavigationSettings& before,
    const NavigationSettings& after);
bool IsFloatingDockShortcutOnlyCommit(
    const DockSettings& before,
    const DockSettings& after) noexcept;

} // namespace snowdesktop::settings_update_rules
