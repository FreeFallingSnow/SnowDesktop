#pragma once

#include "control_settings.h"
#include "settings_controller.h"
#include "winui/home_about_page_model.h"
#include "winui/page_layout_page_presenter.h"

namespace snowdesktop::control_settings
{
struct EnvironmentSettingsContext
{
    ISettingsController& controller;
    winui::PageLayoutPageActions pages;
    std::function<winui::HomeAboutStatusPatch(std::uint64_t, std::uint64_t)> homeAboutStatus;
    std::function<bool()> debugVisible;
};

bool IsEnvironmentSettingsTarget(const Target& target) noexcept;
Value HandleEnvironmentSettings(std::string_view method, const Value& params,
    const Target& target, const EnvironmentSettingsContext& context);
}
