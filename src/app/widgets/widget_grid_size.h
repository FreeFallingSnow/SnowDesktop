#pragma once

#include "app/layout/grid_geometry.h"
#include "widget/packages/widget_size_rules.h"

namespace snowdesktop::widget
{
inline WidgetSizeGridMetrics WidgetSizeMetricsForPage(const GridPage* page, GridCell cell)
{
    if (!page) return StandardWidgetSizeGridMetrics();
    WidgetSizeGridMetrics metrics;
    metrics.columns = std::clamp(page->columns, 1, 8);
    metrics.rows = std::clamp(page->rows, 1, 8);
    for (int span = 1; span <= metrics.columns; ++span)
    {
        const int start = std::clamp(cell.column, 0, std::max(1, page->columns) - span);
        metrics.widths[span - 1] = GetGridAxisOffset(*page, start + span - 1, true) -
            GetGridAxisOffset(*page, start, true) + page->cellWidth;
    }
    for (int span = 1; span <= metrics.rows; ++span)
    {
        const int start = std::clamp(cell.row, 0, std::max(1, page->rows) - span);
        metrics.heights[span - 1] = GetGridAxisOffset(*page, start + span - 1, false) -
            GetGridAxisOffset(*page, start, false) + page->cellHeight;
    }
    return metrics;
}
}
