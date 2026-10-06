#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace snowdesktop
{
// Only the save deadline is retained; the UI serializes its current model at
// the existing persistence boundary. No stale layout snapshot can be replayed.
class LayoutScrollSave
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr unsigned QuietMilliseconds = 250;
    static constexpr unsigned MaximumMilliseconds = 1000;

    unsigned Request(TimePoint now)
    {
        if (!first_) first_ = now;
        due_ = std::min(now + std::chrono::milliseconds(QuietMilliseconds),
            *first_ + std::chrono::milliseconds(MaximumMilliseconds));
        return Delay(now);
    }
    [[nodiscard]] unsigned Delay(TimePoint now) const
    {
        if (!due_ || *due_ <= now) return 1;
        return static_cast<unsigned>(std::chrono::ceil<std::chrono::milliseconds>(
            *due_ - now).count());
    }
    [[nodiscard]] bool Due(TimePoint now) const { return due_ && *due_ <= now; }
    [[nodiscard]] bool Pending() const { return due_.has_value(); }
    void Clear() { first_.reset(); due_.reset(); }
private:
    std::optional<TimePoint> first_;
    std::optional<TimePoint> due_;
};
}
