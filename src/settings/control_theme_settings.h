#pragma once

#include "control_settings.h"
#include "settings_controller.h"
#include <filesystem>
#include <functional>

namespace snowdesktop::themes { struct Theme; }

namespace snowdesktop::control_settings
{
struct ThemeSettingsContext
{
    ISettingsController& controller;
    std::filesystem::path libraryPath;
    // Reuses the host's existing icon-reference reconciliation, when present.
    std::function<bool()> reconcileIconThemes;
    // Called outside the library writer lock. The host resolves the real
    // instance and applies WidgetPatch only with its current mutation guard.
    std::function<bool(std::string_view, const themes::Theme&, const themes::Theme&)> applyWidgetTheme;
};

bool IsThemeSettingsTarget(const Target& target) noexcept;
Value HandleThemeSettings(std::string_view method, const Value& params,
    const Target& target, const ThemeSettingsContext& context);
}
