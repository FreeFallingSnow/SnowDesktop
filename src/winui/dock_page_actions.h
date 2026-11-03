#pragma once

#include "settings/settings_domain.h"
#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "settings/settings_host_actions.h"
#include "theme_library_actions.h"
#include "icons/icon_library.h"

namespace snowdesktop::winui
{

struct DockPageActions
{
    IconLibraryAction iconLibrary;
    std::function<void(std::wstring)> editIconAppearance;
    ThemeLibraryAction themeLibrary;
    ThemeLibraryAsyncAction themeAsync;
    using GeneralEdit = std::function<void(GeneralSettings&)>;
    using DockEdit = std::function<void(DockSettings&)>;
    using ConfirmationCompletion = std::function<void(bool confirmed)>;

    /** Applies an edit to the controller's latest GeneralSettings value. */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        GeneralEdit edit)> updateGeneral;

    /** Applies an edit to the controller's latest DockSettings value. */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        DockEdit edit)> updateDock;

    /** Routes non-setting work through SettingsHostActions. */
    std::function<SettingsActionResult(
        std::uint64_t generation,
        SettingsHostActions::Request request)> invokeHost;

    /** Opens Windows taskbar settings through the owning desktop window. */
    std::function<void(std::uint64_t generation)> openTaskbarSettings;

    /**
     * Shows the host-owned ContentDialog used for dangerous actions.
     * The callback may complete asynchronously on the settings dispatcher.
     */
    std::function<void(
        std::uint64_t generation,
        std::wstring title,
        std::wstring message,
        ConfirmationCompletion completion)> confirm;
};

} // namespace snowdesktop::winui
