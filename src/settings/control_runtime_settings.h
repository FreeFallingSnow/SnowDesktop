#pragma once

#include "control_settings.h"
#include "settings_controller.h"
#include "platform/window_support/window_capabilities.h"
#include "winui/general_advanced_feature_status.h"
#include "winui/home_about_page_model.h"

namespace snowdesktop::control_settings
{
struct RuntimeSettingsContext
{
    ISettingsController& controller;
    std::function<window_support::SupportStatus()> windowSupportStatus;
    std::function<void(bool, HWND)> configureWindowSupport;
    std::function<winui::GeneralAdvancedFeatureStatus()> advancedFeatureStatus;
    std::function<void()> registerAdvancedFeatures;
    std::function<bool()> resetAdvancedFeatures;
    std::function<winui::HomeAboutStatusPatch(std::uint64_t, std::uint64_t)> homeAboutStatus;
    std::function<HWND()> ownerWindow;
    std::function<bool()> debugVisible;
};
bool IsRuntimeSettingsTarget(const Target& target) noexcept;
Value HandleRuntimeSettings(std::string_view method, const Value& params,
    const Target& target, const RuntimeSettingsContext& context);
}
