#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
struct WidgetNetworkInterfaceCounters
{
    std::uint64_t luid = 0;
    std::uint64_t received = 0;
    std::uint64_t sent = 0;
};

struct WidgetNetworkTrafficSample
{
    bool connected = false;
    bool warmingUp = true;
    std::uint64_t receivedBytes = 0;
    std::uint64_t sentBytes = 0;
    std::uint64_t downloadBytesPerSecond = 0;
    std::uint64_t uploadBytesPerSecond = 0;
};

class WidgetNetworkTrafficSampler
{
public:
    using Clock = std::chrono::steady_clock;

    WidgetNetworkTrafficSample Sample(
        std::span<const WidgetNetworkInterfaceCounters> interfaces,
        Clock::time_point now)
    {
        WidgetNetworkTrafficSample result;
        result.connected = !interfaces.empty();
        const double seconds = previousTime_
            ? std::chrono::duration<double>(now - *previousTime_).count() : 0.0;
        std::uint64_t receivedDelta = 0, sentDelta = 0;
        bool measured = false;
        std::unordered_map<std::uint64_t, WidgetNetworkInterfaceCounters> next;
        for (const auto& current : interfaces)
        {
            // Membership changes must never enter the byte delta. New and
            // reset interfaces establish their own baseline for the next tick.
            if (!next.emplace(current.luid, current).second) continue;
            result.receivedBytes += current.received;
            result.sentBytes += current.sent;
            const auto previous = previous_.find(current.luid);
            if (seconds <= 0.0 || previous == previous_.end()) continue;
            if (current.received < previous->second.received ||
                current.sent < previous->second.sent)
                continue;
            measured = true;
            receivedDelta += current.received - previous->second.received;
            sentDelta += current.sent - previous->second.sent;
        }
        if (measured)
        {
            result.downloadBytesPerSecond =
                static_cast<std::uint64_t>(receivedDelta / seconds);
            result.uploadBytesPerSecond =
                static_cast<std::uint64_t>(sentDelta / seconds);
        }
        result.warmingUp = result.connected && !measured;
        previous_ = std::move(next);
        previousTime_ = now;
        return result;
    }

    void Reset()
    {
        previous_.clear();
        previousTime_.reset();
    }

private:
    std::unordered_map<std::uint64_t, WidgetNetworkInterfaceCounters> previous_;
    std::optional<Clock::time_point> previousTime_;
};
}
