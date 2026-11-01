#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace snowdesktop::system_control
{
enum class HotspotManagerState { Unknown, Off, On, InTransition };

// The native backend supplies only the OS state read and manager discovery.
// Keeping this decision on the production path makes lifecycle/invalidation
// checks independent of the machine's network profiles or radio permissions.
template<class Manager>
class HotspotManagerCache
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto AuditInterval = std::chrono::seconds(30);
    static constexpr auto RetryInterval = std::chrono::seconds(5);

    template<class ReadState, class Discover>
    Manager Get(Clock::time_point now, std::uint64_t generation,
        bool notificationsAvailable, bool refreshBeforeEnable,
        ReadState&& readState, Discover&& discover)
    {
        if (manager_)
        {
            const auto state = readState(manager_);
            // Retain an operating hotspot even after its upstream disappears:
            // stopping it must not depend on discovering a new upstream.
            if (state == HotspotManagerState::On || state == HotspotManagerState::InTransition)
                return manager_;
            const auto interval = notificationsAvailable ? AuditInterval : RetryInterval;
            if (state == HotspotManagerState::Off && !refreshBeforeEnable && Fresh(now, generation, interval))
                return manager_;
            Reset();
        }
        else if (!refreshBeforeEnable && Fresh(now, generation, RetryInterval))
            return nullptr; // Bounded retry for a machine without tethering support.

        manager_ = discover();
        verified_ = now;
        generation_ = generation;
        return manager_;
    }

    void Reset()
    {
        manager_ = nullptr;
        verified_.reset();
    }

private:
    bool Fresh(Clock::time_point now, std::uint64_t generation, Clock::duration interval) const
    {
        return verified_ && generation == generation_ && now >= *verified_ && now - *verified_ < interval;
    }

    Manager manager_{nullptr};
    std::optional<Clock::time_point> verified_;
    std::uint64_t generation_ = 0;
};
}
