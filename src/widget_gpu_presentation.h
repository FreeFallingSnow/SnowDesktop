#pragma once
#include "widget_gpu_sampler.h"
#include <algorithm>

namespace snowdesktop::widget_runtime
{
// Deduplicate topology by identity, never by display name or sample validity.
// A real card remains selectable while warming up or temporarily unavailable.
inline std::vector<WidgetGpuAdapterDataSnapshot> PresentGpuAdapters(
    const std::vector<WidgetGpuAdapterDataSnapshot>& source)
{
    const auto usable = [](const auto& item) {
        return item.usageAvailable || item.dedicatedUsageAvailable || item.sharedUsageAvailable;
    };
    std::vector<WidgetGpuAdapterDataSnapshot> result;
    for (const auto& item : source)
    {
        const auto existing = std::find_if(result.begin(), result.end(), [&](const auto& value) {
            return (!item.id.empty() && value.id == item.id) || (item.luid && value.luid == item.luid);
        });
        if (existing != result.end())
        {
            if (!usable(*existing) && usable(item)) *existing = item;
            continue;
        }
        result.push_back(item);
    }
    return result;
}
}
