#pragma once

#include <algorithm>
#include <utility>

#include <dwrite.h>

namespace snowdesktop::widget_runtime
{
struct LayoutMetrics
{
    int gridCellWidth = 92;
    int gridCellHeight = 116;
    int gridGapY = 8;
    int barHeight = 24;
    DWRITE_FONT_WEIGHT itemFontWeight =
        DWRITE_FONT_WEIGHT_SEMI_BOLD;
    float itemFontSizeScale = 1.0f;

    bool operator==(const LayoutMetrics&) const = default;
};

inline LayoutMetrics NormalizeLayoutMetrics(
    int cellWidth, int cellHeight, int gapY, int barHeight,
    DWRITE_FONT_WEIGHT fontWeight, float fontSizeScale)
{
    return {
        std::max(4, cellWidth),
        std::max(4, cellHeight),
        std::max(0, gapY),
        barHeight,
        fontWeight,
        std::max(0.5f, fontSizeScale),
    };
}

template<typename State>
LayoutMetrics CaptureLayoutMetrics(const State& state)
{
    return {
        state.gridCellW,
        state.gridCellH,
        state.gridGapY,
        state.barHeight,
        state.itemFontWeight,
        state.itemFontSizeScale,
    };
}

template<typename State>
void ApplyLayoutMetrics(State& state, const LayoutMetrics& metrics)
{
    state.gridCellW = metrics.gridCellWidth;
    state.gridCellH = metrics.gridCellHeight;
    state.gridGapY = metrics.gridGapY;
    state.barHeight = metrics.barHeight;
    state.itemFontWeight = metrics.itemFontWeight;
    state.itemFontSizeScale = metrics.itemFontSizeScale;
}

template<typename State, typename Activate>
void RestoreLayoutMetrics(State& state, const LayoutMetrics& metrics,
    Activate&& activate)
{
    std::forward<Activate>(activate)();
    ApplyLayoutMetrics(state, metrics);
}
}
