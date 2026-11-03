#pragma once

#include "settings/settings_domain.h"
#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "ui/render/app_font_types.h"
#include "theme_library_actions.h"
#include "shell/shell_extension_service.h"

namespace snowdesktop::winui
{

struct PersonalizationPageActions
{
    std::function<shell_extensions::CatalogueView(const shell_extensions::Request&, bool)> contextMenu;
    std::function<void()> contextMenuClosed;
    using Edit = std::function<void(PersonalizationSettings&)>;
    using GeneralEdit = std::function<void(GeneralSettings&)>;
    using DockEdit = std::function<void(DockSettings&)>;
    using NavigationEdit = std::function<void(NavigationSettings&)>;

    /**
     * Applies an edit to the controller's latest PersonalizationSettings.
     * The presenter deliberately sends no stale settings copy.
     */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        Edit edit)> update;
    /** Theme choices stored in GeneralSettings by the legacy surface. */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        GeneralEdit edit)> updateGeneral;
    std::function<void(std::uint64_t, SettingsUpdateMode, DockEdit)> updateDock;
    std::function<void(std::uint64_t, SettingsUpdateMode, NavigationEdit)> updateNavigation;
    std::function<void(const SettingsRoute&)> navigate;
    std::function<std::vector<app_fonts::Choice>()> listFonts;
    std::function<app_fonts::Selection()> appliedFont;
    std::function<std::vector<app_fonts::Choice>(bool folder, std::string& error)> importFonts;
    std::function<void(std::uint64_t generation)> restartApplication;
    std::function<ThemeLibraryResult(std::uint64_t, const ThemeLibraryRequest&)> themeLibrary;
    std::function<void(std::uint64_t, ThemeLibraryRequest, std::function<void(ThemeLibraryResult)>)> themeAsync;
};

} // namespace snowdesktop::winui
