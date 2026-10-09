#pragma once

#include "dock_window_preview.h"

// Uniform heights, with per-window widths that preserve the DWM source aspect
// ratio and equal side/bottom insets. Each rectangle owns a separate popup.
struct DockWindowPreviewLayout : DockWindowPreviewGrid
{
    std::vector<int> cardWidths;
    std::vector<RECT> cardRects;
};

DockWindowPreviewLayout CalculateDockWindowPreviewLayout(
    const std::vector<SIZE>& sourceSizes, int maximumWidth, int maximumHeight, UINT dpi);
std::vector<RECT> CalculateDockWindowPreviewLayoutCardRects(
    const DockWindowPreviewLayout& layout, UINT dpi);
