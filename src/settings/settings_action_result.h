#pragma once

#include "settings_domain.h"
#include "dock/dock_fullscreen_types.h"

#include <string>
#include <vector>

namespace snowdesktop
{

enum class SettingsActionStatus : std::uint8_t
{
    Succeeded,
    Busy,
    Failed,
};

struct SettingsActionResult
{
    SettingsActionStatus status = SettingsActionStatus::Succeeded;
    SettingsDomain completedDomains = SettingsDomain::None;
    SettingsDomain failedDomains = SettingsDomain::None;
    std::wstring message;
    std::vector<DockFullscreenApplication> dockApplications;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == SettingsActionStatus::Succeeded;
    }

    [[nodiscard]] static SettingsActionResult Success(
        SettingsDomain completed = SettingsDomain::None);
    [[nodiscard]] static SettingsActionResult Busy(std::wstring message);
    [[nodiscard]] static SettingsActionResult Failure(
        std::wstring message,
        SettingsDomain failed = SettingsDomain::None);
};

} // namespace snowdesktop
