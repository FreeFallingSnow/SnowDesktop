#pragma once

#include "constants.h"
#include "dock_layout_settings.h"
#include "icon_beautify.h"

namespace snowdesktop
{
/** Desktop presentation values persisted as part of the layout document. */
struct DesktopDisplaySettings
{
    bool dockEnabled = kDefaultDockEnabled;
    float iconSpacingScale = 1.0f;
    float itemIconSizeScale = kDefaultItemIconSizeScale;
    float itemFontSizeCu = kDefaultItemFontSizeCu;
    float listItemFontSizeCu = kDefaultItemFontSizeCu;
    int itemFontWeight = 600;
    int shortcutArrowMode = 0;
    IconBeautifySettings iconBeautify;
};
} // namespace snowdesktop
