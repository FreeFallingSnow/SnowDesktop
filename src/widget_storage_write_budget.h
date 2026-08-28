#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace snowdesktop::widget_runtime
{
struct StorageWriteBudgetDecision
{
    bool allowed = false;
    std::uint32_t remaining = 0;
    std::chrono::milliseconds retryAfter{};
};

class WidgetStorageWriteBudget final
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::uint32_t kBurstCapacity = 32;
    static constexpr std::chrono::milliseconds kRefillInterval{ 1000 };

    StorageWriteBudgetDecision Consume(Clock::time_point now);

private:
    void Refill(Clock::time_point now);

    Clock::time_point lastRefill_{};
    std::uint32_t available_ = kBurstCapacity;
};
}
