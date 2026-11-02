#pragma once

#include "dock_window_preview.h"

namespace dock_preview_toolbar
{
inline constexpr int kButtonSize = 30;
inline constexpr int kButtonGap = 4;
inline constexpr int kIconSize = 16;
inline constexpr int kBarRegionHeight = 45;
inline constexpr int kBottomInset = 10;
}

// Uniform heights, with per-window widths that preserve the DWM source aspect
// ratio and equal side/bottom insets. Each rectangle owns a separate popup.
struct DockWindowPreviewLayout : DockWindowPreviewGrid
{
    std::vector<int> cardWidths;
    std::vector<RECT> cardRects;
};

DockWindowPreviewLayout CalculateDockWindowPreviewLayout(
    const std::vector<SIZE>& sourceSizes, int maximumWidth, int maximumHeight, UINT dpi,
    const std::vector<size_t>& toolbarButtons = {});
std::vector<RECT> CalculateDockWindowPreviewLayoutCardRects(
    const DockWindowPreviewLayout& layout, UINT dpi);
