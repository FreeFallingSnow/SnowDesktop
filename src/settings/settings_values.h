#pragma once

#include "category_settings_types.h"
#include "desktop/desktop_display_settings.h"
#include "dock/dock_settings_types.h"
#include "general_settings_types.h"
#include "navigation/navigation_settings_types.h"
#include "theme/personalization_types.h"

namespace snowdesktop
{

/** Existing persisted settings, independent of ImGui or WinUI. */
struct SettingsValues
{
    PersonalizationSettings personalization;
    DockSettings dock;
    NavigationSettings navigation;
    GeneralSettings general;
    CategorySettings category = CategorySettings::Defaults();
    DesktopDisplaySettings desktop;
};

} // namespace snowdesktop
