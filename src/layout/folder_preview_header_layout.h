#pragma once

#include "layout/collection_popup_layout.h"

namespace snowdesktop::folder_preview
{
struct NavigationRow
{
    RECT back;
    RECT home;
    RECT breadcrumb;
};

inline int NavigationRowHeight(float scale)
{
    return collection_popup_layout::ScaleDimension(26, scale);
}

inline NavigationRow ResolveNavigationRow(const RECT& frame, LONG top, float scale)
{
    using collection_popup_layout::ScaleDimension;
    const LONG inset = ScaleDimension(4, scale);
    const LONG left = std::min(frame.left + inset, frame.right);
    const LONG right = std::max(left, frame.right - inset);
    top = std::clamp(top, frame.top, frame.bottom);
    const LONG bottom = std::min(frame.bottom, top + NavigationRowHeight(scale));
    const LONG backRight = std::min(right, left + ScaleDimension(28, scale));
    const LONG homeRight = std::min(right, backRight + ScaleDimension(28, scale));
    return {{left, top, backRight, bottom}, {backRight, top, homeRight, bottom}, {homeRight, top, right, bottom}};
}
}
