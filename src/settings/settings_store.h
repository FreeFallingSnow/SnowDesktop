#pragma once

#include "settings_action_result.h"
#include "settings_fwd.h"

#include <memory>

namespace snowdesktop
{

/** Persistence seam used by tests and by the native JSON implementation. */
class SettingsStore
{
public:
    virtual ~SettingsStore() = default;

    virtual SettingsActionResult Load(SettingsValues& values) = 0;
    virtual SettingsActionResult SavePersonalization(
        const PersonalizationSettings& settings) = 0;
    virtual SettingsActionResult SaveDock(
        const DockSettings& settings) = 0;
    virtual SettingsActionResult SaveNavigation(
        const NavigationSettings& settings) = 0;
    virtual SettingsActionResult SaveGeneral(
        const GeneralSettings& settings) = 0;
    virtual SettingsActionResult SaveCategory(
        const CategorySettings& settings) = 0;
};

/** Creates a store backed by the existing SnowDesktop JSON files. */
[[nodiscard]] std::shared_ptr<SettingsStore> CreateNativeSettingsStore();

} // namespace snowdesktop
