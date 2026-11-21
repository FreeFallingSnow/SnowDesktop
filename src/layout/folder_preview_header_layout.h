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
    RECT searchButton;
    RECT searchBox;
    float scale = 1.0f;

    LONG ContentTop() const { return std::max(bounds.bottom, searchBox.bottom); }
};

inline NavigationRow ResolveNavigationRow(const RECT& bounds, float scale,
    bool searchOpen, LONG contentBottom)
{
    using collection_popup_layout::ScaleDimension;
    const auto [left, top, right, bottom] = bounds;
    const int buttonWidth = ScaleDimension(24, scale);
    const LONG backRight = std::min(right, left + buttonWidth);
    const LONG homeRight = std::min(right, backRight + buttonWidth);
    const LONG searchLeft = std::max(homeRight, right - buttonWidth);
    const LONG breadcrumbLeft = std::min(searchLeft, homeRight + ScaleDimension(3, scale));
    const LONG breadcrumbRight = std::max(breadcrumbLeft, searchLeft - ScaleDimension(3, scale));
    const LONG searchTop = bottom + ScaleDimension(collection_popup_layout::kControlGapCu, scale);
    const LONG searchBottom = searchTop + (bottom - top);
    const RECT search = searchOpen && searchBottom <= contentBottom && bottom > top
        ? RECT{left, searchTop, right, searchBottom} : RECT{};
    return {bounds, {left, top, backRight, bottom}, {backRight, top, homeRight, bottom},
        {breadcrumbLeft, top, breadcrumbRight, bottom}, {searchLeft, top, right, bottom}, search, scale};
}
}
