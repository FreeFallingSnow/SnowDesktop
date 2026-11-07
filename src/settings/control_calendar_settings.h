#pragma once

#include "control_settings.h"
#include "settings_fwd.h"
#include "winui/windows_compat.h"
#include "winui/calendar_page_actions.h"

namespace snowdesktop::control_settings
{
struct CalendarSettingsContext
{
    ISettingsController& controller;
    winui::CalendarPageActions calendar;
};
bool IsCalendarSettingsTarget(const Target& target) noexcept;
Value HandleCalendarSettings(std::string_view method, const Value& params,
    const Target& target, const CalendarSettingsContext& context);
}
