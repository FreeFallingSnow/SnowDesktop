#pragma once

#include "settings/settings_domain.h"
#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "theme_library_actions.h"

namespace snowdesktop::winui
{

struct DesktopPageActions
{
    std::function<void(bool)> iconThemeInheritanceChanged;
    ThemeLibraryAction themeLibrary;
    ThemeLibraryAsyncAction themeAsync;
    using DesktopEdit = std::function<void(DesktopDisplaySettings&)>;
    using CategoryEdit = std::function<void(CategorySettings&)>;
    using PersonalizationEdit =
        std::function<void(PersonalizationSettings&)>;

    /**
     * Each edit is applied by the host to the controller's latest domain
     * value.  The presenter intentionally does not send a stale settings
     * object back across the view boundary.
     */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        DesktopEdit edit)> updateDesktop;
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        CategoryEdit edit)> updateCategory;
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        PersonalizationEdit edit)> updatePersonalization;

    /** Persist the Category draft after the explicit Apply action. */
    std::function<void(std::uint64_t generation)> commitCategory;
};

} // namespace snowdesktop::winui
