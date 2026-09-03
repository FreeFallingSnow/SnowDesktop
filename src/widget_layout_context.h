#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

#include <dwrite.h>

namespace snowdesktop::widget_runtime
{
inline constexpr float kReferenceCellWidth = 92.0f;
inline constexpr float kReferenceCellHeight = 116.0f;
inline constexpr float kReferenceCellGap = 8.0f;

inline float ReferenceSpanWidth(int columns) noexcept
{
    const int safeColumns = std::max(1, columns);
    return safeColumns * kReferenceCellWidth +
        (safeColumns - 1) * kReferenceCellGap;
}

inline float ReferenceSpanHeight(int rows) noexcept
{
    const int safeRows = std::max(1, rows);
    return safeRows * kReferenceCellHeight +
        (safeRows - 1) * kReferenceCellGap;
}

inline float ReferenceSpanShortEdge(int columns, int rows) noexcept
{
    return std::min(
        ReferenceSpanWidth(columns), ReferenceSpanHeight(rows));
}

inline float ScaleReferencePixel(float value, float contentWidth,
    float contentHeight, int defaultColumns, int defaultRows) noexcept
{
    const float currentShortEdge = std::max(0.0f,
        std::min(contentWidth, contentHeight));
    const float referenceShortEdge = ReferenceSpanShortEdge(
        defaultColumns, defaultRows);
    return value * currentShortEdge / referenceShortEdge;
}

inline float ScaleReferenceAxis(float value, float currentExtent,
    float referenceExtent) noexcept
{
    return value * std::max(0.0f, currentExtent) /
        std::max(1.0f, referenceExtent);
}

struct LayoutMetrics
{
    int gridCellWidth = 92;
    int gridCellHeight = 116;
    int gridGapY = 8;
    int barHeight = 24;
    DWRITE_FONT_WEIGHT itemFontWeight =
        DWRITE_FONT_WEIGHT_SEMI_BOLD;

    bool operator==(const LayoutMetrics&) const = default;
};

inline LayoutMetrics NormalizeLayoutMetrics(
    int cellWidth, int cellHeight, int gapY, int barHeight,
    DWRITE_FONT_WEIGHT fontWeight)
{
    return {
        std::max(4, cellWidth),
        std::max(4, cellHeight),
        std::max(0, gapY),
        barHeight,
        fontWeight,
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
}

template<typename State, typename Activate>
void RestoreLayoutMetrics(State& state, const LayoutMetrics& metrics,
    Activate&& activate)
{
    std::forward<Activate>(activate)();
    ApplyLayoutMetrics(state, metrics);
}
}
