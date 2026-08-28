#include "widget_storage_write_budget.h"

#include <cstdlib>
#include <iostream>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestBurstAndRefill()
{
    using Budget =
        snowdesktop::widget_runtime::WidgetStorageWriteBudget;
    Budget budget;
    const auto start = Budget::Clock::time_point(std::chrono::seconds(10));
    for (std::uint32_t index = 0; index < Budget::kBurstCapacity; ++index)
    {
        const auto decision = budget.Consume(start);
        Check(decision.allowed &&
                decision.remaining == Budget::kBurstCapacity - index - 1,
            "the initial burst must be available immediately");
    }
    auto blocked = budget.Consume(start);
    Check(!blocked.allowed &&
            blocked.retryAfter == Budget::kRefillInterval,
        "a depleted budget reports the next refill delay");

    blocked = budget.Consume(start + std::chrono::milliseconds(999));
    Check(!blocked.allowed &&
            blocked.retryAfter == std::chrono::milliseconds(1),
        "sub-second retry diagnostics remain precise");
    const auto refilled = budget.Consume(
        start + Budget::kRefillInterval);
    Check(refilled.allowed && refilled.remaining == 0,
        "one token refills after one interval");
}

void TestCapacityAndMonotonicClockHandling()
{
    using Budget =
        snowdesktop::widget_runtime::WidgetStorageWriteBudget;
    Budget budget;
    const auto start = Budget::Clock::time_point(std::chrono::seconds(10));
    Check(budget.Consume(start).allowed,
        "the budget starts full");
    const auto muchLater = start + std::chrono::hours(1);
    for (std::uint32_t index = 0; index < Budget::kBurstCapacity; ++index)
    {
        Check(budget.Consume(muchLater).allowed,
            "a long idle period refills only to burst capacity");
    }
    Check(!budget.Consume(muchLater).allowed,
        "idle time cannot bank more than the burst capacity");
    Check(!budget.Consume(start).allowed,
        "a backwards clock observation cannot refill the budget");
}
}

int main()
{
    TestBurstAndRefill();
    TestCapacityAndMonotonicClockHandling();
    if (failures != 0)
    {
        std::cerr << failures << " widget storage write budget checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget storage write budget checks passed\n";
    return EXIT_SUCCESS;
}
