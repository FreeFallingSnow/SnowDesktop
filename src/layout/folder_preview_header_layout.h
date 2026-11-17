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

inline HeaderLayout ResolveHeader(const RECT& frame, float scale, RECT collapseButton = {})
{
    using collection_popup_layout::ScaleDimension;
    const int width = std::max(0, static_cast<int>(frame.right - frame.left));
    const int inset = std::min(ScaleDimension(12, scale), width / 10);
    const int gap = std::min(ScaleDimension(4, scale), width / 24);
    const LONG right = frame.right - inset;
    const LONG left = std::clamp(collapseButton.right > collapseButton.left
        ? std::max<LONG>(frame.left + inset, collapseButton.right + gap)
        : frame.left + inset, frame.left, std::max(frame.left, right - 3 * gap));
    const int available = std::max(0, static_cast<int>(right - left) - 3 * gap);
    const int title = std::min(ScaleDimension(120, scale), available / 4);
    const int buttons = available - title;
    const int backWidth = std::min(ScaleDimension(60, scale), buttons / 4);
    const int sortWidth = std::min(ScaleDimension(82, scale), buttons / 3);
    const int explorerWidth = std::min(ScaleDimension(144, scale), buttons - backWidth - sortWidth);
    const LONG top = frame.top + ScaleDimension(11, scale);
    const LONG bottom = top + ScaleDimension(30, scale);
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
