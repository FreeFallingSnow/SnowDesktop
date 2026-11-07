#include "settings_action_result.h"

#include <utility>

namespace snowdesktop
{

SettingsActionResult SettingsActionResult::Success(SettingsDomain completed)
{
    SettingsActionResult result;
    result.completedDomains = completed;
    return result;
}

SettingsActionResult SettingsActionResult::Busy(std::wstring message)
{
    SettingsActionResult result;
    result.status = SettingsActionStatus::Busy;
    result.message = std::move(message);
    return result;
}

SettingsActionResult SettingsActionResult::Failure(
    std::wstring message,
    SettingsDomain failed)
{
    SettingsActionResult result;
    result.status = SettingsActionStatus::Failed;
    result.failedDomains = failed;
    result.message = std::move(message);
    return result;
}

} // namespace snowdesktop
