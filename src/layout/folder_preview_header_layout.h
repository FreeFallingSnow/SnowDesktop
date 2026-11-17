#pragma once

#include "layout/collection_popup_layout.h"

namespace snowdesktop::folder_preview
{
struct HeaderLayout
{
    RECT back;
    RECT title;
    RECT explorer;
    RECT sort;
};

inline HeaderLayout ResolveHeader(const RECT& frame, float scale)
{
    using collection_popup_layout::ScaleDimension;
    const int width = std::max(0, static_cast<int>(frame.right - frame.left));
    const int inset = std::min(ScaleDimension(12, scale), width / 10);
    const int gap = std::min(ScaleDimension(4, scale), width / 24);
    const int available = width - 2 * inset - 3 * gap;
    const int title = std::min(ScaleDimension(120, scale), available / 4);
    const int buttons = available - title;
    const int backWidth = std::min(ScaleDimension(60, scale), buttons / 4);
    const int sortWidth = std::min(ScaleDimension(82, scale), buttons / 3);
    const int explorerWidth = std::min(ScaleDimension(144, scale), buttons - backWidth - sortWidth);
    const LONG top = frame.top + ScaleDimension(11, scale);
    const LONG bottom = top + ScaleDimension(30, scale);
    const LONG left = frame.left + inset;
    const LONG right = frame.right - inset;
    const LONG explorerRight = right - sortWidth - gap;
    const LONG explorerLeft = explorerRight - explorerWidth;
    return {
        {left, top, left + backWidth, bottom},
        {left + backWidth + gap, top, explorerLeft - gap, bottom},
        {explorerLeft, top, explorerRight, bottom},
        {right - sortWidth, top, right, bottom}
    };
}
}
