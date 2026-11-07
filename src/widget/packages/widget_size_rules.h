#pragma once

#include "common/json_value.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace snowdesktop::widget
{
struct AspectGridSize
{
    double ratio = 1.0; // outer width / outer height
    bool fixedRows = true;
};

struct WidgetSizeDefinition
{
    int columns = 1;
    int rows = 1;
    std::optional<AspectGridSize> aspect;
};

struct WidgetSizeGridMetrics
{
    int columns = 8;
    int rows = 8;
    std::array<double, 8> widths{};
    std::array<double, 8> heights{};
};

inline WidgetSizeGridMetrics StandardWidgetSizeGridMetrics()
{
    WidgetSizeGridMetrics metrics;
    for (int i = 0; i < 8; ++i)
    {
        metrics.widths[i] = 92.0 * (i + 1) + 8.0 * i;
        metrics.heights[i] = 116.0 * (i + 1) + 8.0 * i;
    }
    return metrics;
}

// Integer spans remain the only placement/rendering representation. A ratio
// definition is resolved once against the destination page before placement.
inline std::pair<int, int> ResolveWidgetSize(const WidgetSizeDefinition& size,
    const WidgetSizeGridMetrics& metrics, int minColumns = 1, int minRows = 1,
    int maxColumns = 0, int maxRows = 0)
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
            best = {c, r}; bestError = error; bestRatio = ratio;
        }
    }
    return best;
}

// Shared by directory validation and runtime metadata loading. Legacy grid
// sizes keep their existing parser; only the new ratio form is strict here.
inline bool ReadAspectGridSize(const JsonValue* value, int& columns, int& rows,
    std::optional<AspectGridSize>& aspect, std::string& error)
{
    aspect.reset();
    if (!value || !value->IsObject() || !value->Find("aspectRatio")) return true;
    const auto* ratio = value->Find("aspectRatio");
    const auto* c = value->Find("columns");
    const auto* r = value->Find("rows");
    const auto* fixed = r ? r : c;
    if (!ratio->IsNumber() || !std::isfinite(ratio->number) || ratio->number <= 0 ||
        (c != nullptr) == (r != nullptr) || !fixed || !fixed->IsNumber() ||
        !std::isfinite(fixed->number) || fixed->number != std::floor(fixed->number) ||
        fixed->number < 1 || fixed->number > 8)
    {
        error = "aspectRatio must be a positive finite width/height ratio with exactly one integer rows or columns from 1 to 8";
        return false;
    }
    aspect = AspectGridSize{ratio->number, r != nullptr};
    if (r) rows = static_cast<int>(r->number);
    else columns = static_cast<int>(c->number);
    const auto resolved = ResolveWidgetSize({columns, rows, aspect}, StandardWidgetSizeGridMetrics());
    columns = resolved.first; rows = resolved.second;
    return true;
}
}
