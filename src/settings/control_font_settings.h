#pragma once

#include "control_settings.h"
#include "settings_controller.h"
#include "ui/render/app_font.h"

namespace snowdesktop::control_settings
{
struct FontSettingsContext
{
    ISettingsController& controller;
    std::function<std::vector<app_fonts::Choice>()> list;
    std::function<bool(const std::filesystem::path&, std::vector<app_fonts::Choice>&, std::string&)> import;
    std::function<app_fonts::Selection()> applied;
};
bool IsFontSettingsTarget(const Target& target) noexcept;
Value HandleFontSettings(std::string_view method, const Value& params,
    const Target& target, const FontSettingsContext& context);
}
