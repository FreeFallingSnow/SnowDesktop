#pragma once
#include "control_settings.h"
#include "icons/icon_appearance.h"
#include "icons/icon_library.h"
#include "icons/large_icon_settings.h"
#include <optional>

namespace snowdesktop::control_settings
{
struct IconSettingsState
{
    std::optional<LargeIconSettingsSnapshot> largeIcon;
    std::optional<IconLibrarySnapshot> library;
};
struct IconSettingsContext
{
    IconAppearanceAction appearance;
    IconLibraryAction library;
    LargeIconSettingsAction largeIcon;
    IconSettingsState& state;
    // Large-icon editor sessions are exclusive. Never replace a visible
    // settings editor's pending preview with an automation session.
    std::function<bool()> editingBusy;
    std::function<bool(std::wstring_view)> restoreLargeIcon;
};
bool IsIconSettingsTarget(const Target& target) noexcept;
Value HandleIconSettings(std::string_view method, const Value& params,
    const Target& target, const IconSettingsContext& context);
}
