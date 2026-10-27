#pragma once
#include "window_support_protocol.h"
#include <algorithm>

namespace snowdesktop::window_support
{
class RecentPointerEvents
{
public:
    bool Accept(const protocol::PointerEvent& event, DWORD now) noexcept
    {
        if (now - event.time > 250) return false;
        const auto duplicate = [&](const auto& old) {
            return old.time == event.time && old.message == event.message && old.mouseData == event.mouseData &&
                old.flags == event.flags && old.point.x == event.point.x && old.point.y == event.point.y;
        };
        if (std::any_of(events_.begin(), events_.begin() + count_, duplicate)) return false;
        events_[next_++ % events_.size()] = event;
        count_ = (std::min)(count_ + 1, events_.size());
        return true;
    }
private:
    std::array<protocol::PointerEvent, 128> events_{};
    std::size_t count_ = 0, next_ = 0;
};
}
