#pragma once

#include <optional>
#include <string_view>

namespace snowdesktop::widget_runtime
{
std::optional<std::wstring_view> SystemSettingsUri(
    std::string_view page) noexcept;

// In-app settings page opened by system.openSettings. The value is the
// stable SettingsPageKey, not a Windows settings URI.
std::optional<std::string_view> HostSettingsPage(
    std::string_view page) noexcept;
}
