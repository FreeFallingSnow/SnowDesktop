#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace snowdesktop::widget_runtime
{
class WidgetStorageBusyAccumulator
{
public:
    void AddIdleSample(std::wstring_view instance, double idlePercent)
    {
        if (instance.empty() || instance == L"_Total" || instance == L"_total" ||
            !std::isfinite(idlePercent) || idlePercent < 0.0)
            return;
        const double busy = 100.0 - std::clamp(idlePercent, 0.0, 100.0);
        busiest_ = std::max(busiest_.value_or(0.0), busy);
    }

    std::optional<double> BusyPercent() const { return busiest_; }

private:
    std::optional<double> busiest_;
};
}
