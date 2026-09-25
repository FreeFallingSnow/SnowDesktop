#pragma once

#include "types.h"

#include <optional>
#include <vector>

namespace snowdesktop::widget_runtime
{
// The prompt can dispatch messages that reorder/remove desktop widgets. Return
// a fresh index only for the original instance, without mutating layout/storage.
template <typename Confirm>
std::optional<std::size_t> ConfirmWidgetRemoval(
    const std::vector<DesktopWidget>& widgets, std::size_t index,
    bool requiresConfirmation, Confirm&& confirm)
{
    if (index >= widgets.size()) return std::nullopt;
    const auto id = widgets[index].id;
    const auto packageId = widgets[index].packageId;
    const auto type = widgets[index].type;
    const auto title = widgets[index].title;
    if (requiresConfirmation && !confirm(title)) return std::nullopt;
    for (std::size_t current = 0; current < widgets.size(); ++current)
    {
        if (widgets[current].id == id && widgets[current].type == type &&
            widgets[current].packageId == packageId)
            return current;
    }
    return std::nullopt;
}
}
