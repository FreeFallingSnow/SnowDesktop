#pragma once

#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>

namespace snowdesktop::winui
{

struct ScreenTimePageActions
{
    using GeneralEdit = std::function<void(GeneralSettings&)>;
    std::function<void(std::uint64_t, GeneralEdit)> commitGeneral;
    std::function<bool(std::uint64_t)> clear;
};

} // namespace snowdesktop::winui
