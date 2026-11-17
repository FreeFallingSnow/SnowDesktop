#pragma once

#include "common/utils.h"
#include "layout/collection_popup_layout.h"
#include "widgets/collection_group_rules.h"

namespace snowdesktop::folder_preview
{
struct NavigationRow
{
    RECT bounds;
    RECT back;
    RECT home;
    RECT breadcrumb;
    float scale = 1.0f;
};

inline NavigationRow ResolveNavigationRow(const RECT& bounds, float scale)
{
    using collection_popup_layout::ScaleDimension;
    const auto [left, top, right, bottom] = bounds;
    const int buttonWidth = ScaleDimension(24, scale);
    const LONG backRight = std::min(right, left + buttonWidth);
    const LONG homeRight = std::min(right, backRight + buttonWidth);
    const LONG breadcrumbLeft = std::min(right, homeRight + ScaleDimension(3, scale));
    return {bounds, {left, top, backRight, bottom}, {backRight, top, homeRight, bottom},
        {breadcrumbLeft, top, right, bottom}, scale};
}
}
