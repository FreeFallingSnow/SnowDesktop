#pragma once

#include "ui/render/app_font_types.h"
#include "control_settings.h"
#include "settings_fwd.h"
#include <functional>

namespace snowdesktop::control_settings
{
struct GlobalSettingsContext
{
    ISettingsController& controller;
    std::function<bool(std::string_view)> languageAvailable;
    std::function<bool(const app_fonts::Selection&)> fontAvailable;
};
bool IsGlobalSettingsTarget(const Target& target);
Value HandleGlobalSettings(std::string_view method, const Value& params,
    const Target& target, const GlobalSettingsContext& context);
}
