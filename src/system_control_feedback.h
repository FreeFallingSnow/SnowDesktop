#pragma once
#include "system_controls.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace snowdesktop::system_control
{
struct BrightnessReadback
{
    std::optional<double> value;
    Result failure;
};
// Only the device read and clock wait are replaceable in tests. A successful
// write is not sufficient: observe the requested value after hardware settles.
template<class Read, class Pause>
Result ConfirmBrightness(double target, double tolerance, const Cancellation& cancel,
    Read&& read, Pause&& pause)
{
    for (unsigned attempt = 0; attempt < 20; ++attempt)
    {
        if (cancel.Stop()) return cancel.Failure();
        const auto actual = read();
        if (cancel.Stop()) return cancel.Failure();
        if (!actual.value) return actual.failure;
        if (std::isfinite(*actual.value) && std::abs(*actual.value - target) <= tolerance)
            return {true, {}, 0};
        if (attempt != 19) pause();
    }
    return {false, "stateMismatch", 0};
}
inline void BrightnessReadbackPause()
{ std::this_thread::sleep_for(std::chrono::milliseconds(100)); }

// UI feedback belongs to the newest request for this control and device, even
// when older completions were already queued before a replacement was issued.
class ControlFeedback
{
    std::map<std::string, std::uint64_t> latest_;
public:
    static std::string Key(const Request& request)
    {
        std::string result = request.name;
        for (const auto* field : {"monitorId", "endpointId", "interfaceId", "deviceId", "radioId", "sessionId"})
            if (const auto it = request.arguments.find(field); it != request.arguments.end())
                result += ":" + std::string(field) + ":" + std::to_string(it->second.size()) + ":" + it->second;
        return result;
    }
    void Track(std::string key, std::uint64_t id)
    { if (id) latest_[std::move(key)] = id; else latest_.erase(key); }
    bool Take(std::uint64_t id)
    {
        const auto found = std::find_if(latest_.begin(), latest_.end(),
            [id](const auto& entry) { return entry.second == id; });
        if (found == latest_.end()) return false;
        latest_.erase(found); return true;
    }
    void Clear() { latest_.clear(); }
};
}
