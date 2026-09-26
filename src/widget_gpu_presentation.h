#pragma once
#include "widget_gpu_sampler.h"
#include <algorithm>

namespace snowdesktop::widget_runtime
{
// Topology aliases with no independent samples must not repeat a working GPU.
// Distinct usable adapters of the same model remain selectable by identity.
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
            return value.id == item.id || (item.luid && value.luid == item.luid);
        });
        if (existing != result.end())
        {
            if (!usable(*existing) && usable(item)) *existing = item;
            continue;
        }
        if (!usable(item) && std::any_of(source.begin(), source.end(), [&](const auto& value) {
                return value.name == item.name && usable(value);
            })) continue;
        if (!usable(item) && std::any_of(result.begin(), result.end(), [&](const auto& value) {
                return value.name == item.name && !usable(value);
            })) continue;
        result.push_back(item);
    }
    return result;
}
}
