#include "widget_storage_write_budget.h"

#include <algorithm>

namespace snowdesktop::widget_runtime
{
void WidgetStorageWriteBudget::Refill(Clock::time_point now)
{
    if (lastRefill_ == Clock::time_point{})
    {
        lastRefill_ = now;
        return;
    }
    if (now <= lastRefill_) return;

    const auto elapsed = now - lastRefill_;
    const auto replenished = static_cast<std::uint64_t>(
        elapsed / kRefillInterval);
    if (replenished == 0) return;
    const auto missing = static_cast<std::uint64_t>(
        kBurstCapacity - available_);
    available_ += static_cast<std::uint32_t>(
        std::min(replenished, missing));
    if (available_ == kBurstCapacity)
        lastRefill_ = now;
    else
        lastRefill_ += kRefillInterval *
            static_cast<std::int64_t>(replenished);
}

StorageWriteBudgetDecision WidgetStorageWriteBudget::Consume(
    Clock::time_point now)
{
    Refill(now);
    if (available_ > 0)
    {
        --available_;
        return { true, available_, std::chrono::milliseconds::zero() };
    }

    const auto next = lastRefill_ + kRefillInterval;
    auto retryAfter = std::chrono::duration_cast<std::chrono::milliseconds>(
        next > now ? next - now : Clock::duration::zero());
    if (retryAfter < std::chrono::milliseconds(1))
        retryAfter = std::chrono::milliseconds(1);
    return { false, 0, retryAfter };
}
}
