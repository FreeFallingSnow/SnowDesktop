#pragma once

#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>

namespace snowdesktop
{
struct SettingsActionResult;
}

namespace snowdesktop::winui
{

struct ScreenTimePageActions
{
    using GeneralEdit = std::function<void(GeneralSettings&)>;
    using CommitGeneral = std::function<void(std::uint64_t, GeneralEdit)>;
    CommitGeneral commitGeneral;
    std::function<bool(std::uint64_t)> clear;
};

ScreenTimePageActions MakeScreenTimePageActions(
    ScreenTimePageActions::CommitGeneral commitGeneral,
    std::function<ISettingsController*()> currentController,
    std::function<void(const SettingsActionResult&)> showResult);

} // namespace snowdesktop::winui
