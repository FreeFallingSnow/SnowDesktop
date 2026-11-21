#include "widget_size_rules.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace snowdesktop::widget
{
WidgetSizeGridMetrics StandardWidgetSizeGridMetrics()
{
    WidgetSizeGridMetrics metrics;
    for (int i = 0; i < 8; ++i)
    {
        metrics.widths[i] = 92.0 * (i + 1) + 8.0 * i;
        metrics.heights[i] = 116.0 * (i + 1) + 8.0 * i;
    }
    return metrics;
}

std::pair<int, int> ResolveWidgetSize(const WidgetSizeDefinition& size,
    const WidgetSizeGridMetrics& metrics, int minColumns, int minRows,
    int maxColumns, int maxRows)
{
    if (!size.aspect) return {size.columns, size.rows};
    int upperC = std::clamp(metrics.columns, 1, 8);
    int upperR = std::clamp(metrics.rows, 1, 8);
    if (maxColumns > 0) upperC = std::min(upperC, maxColumns);
    if (maxRows > 0) upperR = std::min(upperR, maxRows);
    const int lowerC = std::clamp(minColumns, 1, upperC);
    const int lowerR = std::clamp(minRows, 1, upperR);
    const int columns = std::clamp(size.columns, lowerC, upperC);
    const int rows = std::clamp(size.rows, lowerR, upperR);
    std::pair<int, int> best{columns, rows};
    double bestError = (std::numeric_limits<double>::max)();
    double bestRatio = 0;
    const bool fixedRows = size.aspect->fixedRows;
    for (int candidate = fixedRows ? lowerC : lowerR;
        candidate <= (fixedRows ? upperC : upperR); ++candidate)
    {
        const int c = fixedRows ? candidate : columns;
        const int r = fixedRows ? rows : candidate;
        const double w = metrics.widths[c - 1], h = metrics.heights[r - 1];
        if (!(w > 0 && h > 0)) continue;
        const double ratio = w / h;
        const double error = std::abs(ratio - size.aspect->ratio);
        const bool sameSideCloser = error == bestError &&
            ((ratio < size.aspect->ratio && bestRatio < ratio) ||
             (ratio > size.aspect->ratio && bestRatio > ratio));
        if (error < bestError || sameSideCloser)
        {
            best = {c, r};
            bestError = error;
            bestRatio = ratio;
        }
    }
    return best;
}
}
