#pragma once

#include "dock_window_preview.h"

// Uniform widths, with per-window heights that preserve the DWM source aspect
// ratio and equal side/bottom insets. Kept outside the app coordination header.
struct DockWindowPreviewLayout : DockWindowPreviewGrid
{
    std::vector<int> cardHeights;
};

DockWindowPreviewLayout CalculateDockWindowPreviewLayout(
    const std::vector<SIZE>& sourceSizes, int maximumWidth, int maximumHeight, UINT dpi);
std::vector<RECT> CalculateDockWindowPreviewLayoutCardRects(
    const DockWindowPreviewLayout& layout, UINT dpi);
