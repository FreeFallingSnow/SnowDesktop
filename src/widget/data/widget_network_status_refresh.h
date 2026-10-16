#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace snowdesktop::widget_runtime
{
// The worker still samples/publishes at the requested interval. An OS-event
// maintained status needs a heavyweight refresh only on invalidation or audit.
class WidgetNetworkStatusRefresh
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto AuditInterval = std::chrono::seconds(30);

    bool Due(Clock::time_point now, std::uint64_t generation,
        bool notificationsAvailable) const noexcept
    {
        return !notificationsAvailable || !verified_ || generation != generation_ ||
            now < *verified_ || now - *verified_ >= AuditInterval;
    }
    void Complete(Clock::time_point now, std::uint64_t generation, bool available) noexcept
    {
        generation_ = generation;
        if (available) verified_ = now;
        else verified_.reset();
    }

private:
    std::optional<Clock::time_point> verified_;
    std::uint64_t generation_ = 0;
};
}
