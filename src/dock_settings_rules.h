#pragma once

namespace snowdesktop::dock_settings_rules
{

inline void NormalizeAlwaysEnabledFeatures(
    bool& showRunningApps,
    bool& showWindowPreviews) noexcept
{
    showRunningApps = true;
    showWindowPreviews = true;
}

} // namespace snowdesktop::dock_settings_rules
