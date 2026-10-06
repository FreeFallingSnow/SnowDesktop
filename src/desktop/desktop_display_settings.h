#pragma once

#include "common/constants.h"
#include "ui/render/font_weight_rules.h"
#include "dock/dock_layout_settings.h"
#include "icons/icon_beautify.h"

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
    int itemFontWeight = font_weight_rules::kDefaultWeight;
    int desktopTitleLines = 2;
    int largeFolderTitleLines = 2;
    int scrollingTitleLines = 2;
    bool titleEllipsis = true;
    int shortcutArrowMode = 0;
    IconBeautifySettings iconBeautify;
};
} // namespace snowdesktop
