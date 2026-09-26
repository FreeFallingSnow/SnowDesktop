#pragma once
#include "native_tooltip_state.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop
{
// Widths are presentation contracts, independent of a sample's digits. The
// localized memory label is reserved using its widest percentage separately.
inline float StatusBarFixedWidth(std::string_view key)
{
    if (key == "cpu" || key == "gpu") return 68.f;
    if (key == "traffic") return 152.f;
    if (key == "controlCenter") return 136.f;
    return 0.f;
}
inline std::wstring StatusBarRate(std::uint64_t bytes)
{
    constexpr const wchar_t* units[]{L"B/s", L"KiB/s", L"MiB/s", L"GiB/s", L"TiB/s"};
    double value = static_cast<double>(bytes);
    unsigned unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    const auto tenths = static_cast<unsigned long long>(std::llround(std::min(value, 999.9) * 10));
    return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" " + units[unit];
}
// Accumulate high-resolution wheel deltas and rapid steps against the pending
// target, not against a stale asynchronous device sample.
struct StatusBarVolumeWheel
{
    int remainder = 0;
    std::optional<double> pending;
    std::uint64_t task = 0;
    std::string endpoint;
    std::optional<double> Move(int delta, double sampled)
    {
        remainder += delta;
        const int steps = remainder / 120;
        remainder %= 120;
        if (!steps || !std::isfinite(sampled)) return {};
        pending = std::clamp(pending.value_or(sampled) + steps * .02, 0., 1.);
        return pending;
    }
    void Started(std::uint64_t id, std::string device)
    {
        task = id; endpoint = std::move(device);
        if (!id) Reset();
    }
    bool Complete(std::uint64_t id)
    {
        if (!task || id != task) return false;
        Reset(); return true;
    }
    void Reset() { remainder = 0; pending.reset(); task = 0; endpoint.clear(); }
};
using StatusBarTooltipState = NativeTooltipState;
}
