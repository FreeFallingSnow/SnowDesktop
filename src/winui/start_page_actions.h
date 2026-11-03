#pragma once

#include "settings/settings_domain.h"
#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "platform/usage_guide.h"
#include "settings/settings_search_index.h"

namespace snowdesktop::winui
{

struct StartPageActions
{
    std::function<void(std::uint64_t, usage_guide::Topic)> begin;
    std::function<void(std::uint64_t, bool)> expandedChanged;
    std::function<void(const SettingsRoute&)> navigate;
    std::function<std::vector<StaticSettingSearchDescriptor>()> settingsIndex;
};

} // namespace snowdesktop::winui
