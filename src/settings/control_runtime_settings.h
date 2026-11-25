#pragma once

#include "control_settings.h"
#include "settings_fwd.h"
#include "platform/window_support/window_capabilities.h"
#include "winui/general_advanced_feature_status.h"
#include "home_about_status.h"

namespace snowdesktop::control_settings
{
inline constexpr auto DesktopStyleAction = "apply-desktop-style";
inline constexpr auto DesktopStyleAnimationAction = "apply-style-animations";
inline constexpr auto DesktopStyleActionDescription =
    "Apply a desktop layout preset to the Dock, status bar and Windows taskbar. Compare the preset's bar positions, taskbar behavior and reserved space before applying.";
inline constexpr auto DesktopStyleAnimationActionDescription =
    "Apply the selected desktop style's recommended Dock icon hover and launch animations. Window minimize and restore effects are controlled separately by dock.windowEffect.";

struct RuntimeSettingsContext
{
    ISettingsController& controller;
    std::function<window_support::SupportStatus()> windowSupportStatus;
    std::function<void(bool, HWND)> configureWindowSupport;
    std::function<winui::GeneralAdvancedFeatureStatus()> advancedFeatureStatus;
    std::function<void()> registerAdvancedFeatures;
    std::function<bool()> resetAdvancedFeatures;
    std::function<settings::HomeAboutStatusPatch(std::uint64_t, std::uint64_t)> homeAboutStatus;
    std::function<HWND()> ownerWindow;
    std::function<bool()> debugVisible;
};
bool IsRuntimeSettingsTarget(const Target& target) noexcept;
Value HandleRuntimeSettings(std::string_view method, const Value& params,
    const Target& target, const RuntimeSettingsContext& context);
}
