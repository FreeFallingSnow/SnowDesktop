#pragma once

#include "common/types.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_groups
{
inline std::optional<std::size_t> FindParent(const std::vector<DesktopWidget>& widgets,
    std::wstring_view child, DesktopWidgetType groupType)
{
    for (std::size_t i = 0; i < widgets.size(); ++i)
    {
        const auto& group = widgets[i];
        if (group.type == groupType && std::find(group.childWidgetIds.begin(),
                group.childWidgetIds.end(), child) != group.childWidgetIds.end())
            return i;
    }
    return std::nullopt;
}
}
