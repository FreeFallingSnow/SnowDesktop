#pragma once

#include <optional>

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
}
