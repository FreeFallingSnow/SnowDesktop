#pragma once

#include "widget_size_types.h"

#include <array>
#include <utility>

namespace snowdesktop::widget
{
struct WidgetSizeGridMetrics
{
    int columns = 8;
    int rows = 8;
    std::array<double, 8> widths{};
    std::array<double, 8> heights{};
};

WidgetSizeGridMetrics StandardWidgetSizeGridMetrics();

// Integer spans remain the only placement/rendering representation. A ratio
// definition is resolved once against the destination page before placement.
std::pair<int, int> ResolveWidgetSize(const WidgetSizeDefinition& size,
    const WidgetSizeGridMetrics& metrics, int minColumns = 1, int minRows = 1,
    int maxColumns = 0, int maxRows = 0);
}
