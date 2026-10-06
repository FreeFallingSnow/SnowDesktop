#pragma once

#include <cmath>

namespace snowdesktop::winui::number_box_update_rules
{
// NumberBox formats its inner TextBox even when Value is assigned the same
// number. Snapshot refreshes must leave an unfinished text edit untouched.
inline bool ShouldWriteValue(
    double current, double incoming, bool editing = false) noexcept
{
    return !editing && current != incoming &&
        !(std::isnan(current) && std::isnan(incoming));
}
}
