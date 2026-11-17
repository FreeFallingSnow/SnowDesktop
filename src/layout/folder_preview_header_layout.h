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

inline int NavigationRowHeight(float tabHeightCu, float scale)
{
    return ScaleWidgetCu(collection_group_rules::ResolveCategorizedControlMetrics(tabHeightCu).searchBoxHeightCu, scale);
}

inline constexpr int kPopupNavigationTitleInsetCu = 7;
inline constexpr int kPopupNavigationBottomGapCu = 6;

inline NavigationRow ResolveNavigationRow(const RECT& frame, LONG top, float tabHeightCu, float scale)
{
    using collection_popup_layout::ScaleDimension;
    const LONG inset = ScaleDimension(12, scale);
    const LONG left = std::min(frame.left + inset, frame.right);
    const LONG right = std::max(left, frame.right - inset);
    top = std::clamp(top, frame.top, frame.bottom);
    const LONG bottom = std::min(frame.bottom, top + NavigationRowHeight(tabHeightCu, scale));
    const LONG backRight = std::min(right, left + ScaleDimension(29, scale));
    const LONG homeRight = std::min(right, backRight + ScaleDimension(29, scale));
    const LONG breadcrumbLeft = std::min(right, homeRight + ScaleDimension(4, scale));
    return {{left, top, right, bottom}, {left, top, backRight, bottom}, {backRight, top, homeRight, bottom},
        {breadcrumbLeft, top, right, bottom}, scale};
}
}
