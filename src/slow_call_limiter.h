#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace snowdesktop
{
enum class SlowCallPhase : std::size_t { Message, Composition, QuickNavigation, Due, Count };
struct SlowCallSuppression { std::uint64_t count = 0; double maximumMs = 0; };
// The message-loop owner is the sole caller. Four fixed slots add no allocation
// or cross-thread ownership to the synchronous diagnostic sink.
class SlowCallLimiter
{
public:
    std::optional<SlowCallSuppression> Record(SlowCallPhase phase, double now, double elapsed)
    {
        auto& slot = slots_[static_cast<std::size_t>(phase)];
        if (!slot.emitted || now < slot.last || now - slot.last >= 1000.0)
        {
            const auto result = slot.pending;
            slot.pending = {}; slot.last = now; slot.emitted = true;
            return result;
        }
        if (slot.pending.count < (std::numeric_limits<std::uint64_t>::max)()) ++slot.pending.count;
        if (elapsed > slot.pending.maximumMs) slot.pending.maximumMs = elapsed;
        return std::nullopt;
    }
    std::optional<SlowCallSuppression> Flush(SlowCallPhase phase)
    {
        auto& pending = slots_[static_cast<std::size_t>(phase)].pending;
        if (!pending.count) return std::nullopt;
        const auto result = pending; pending = {}; return result;
    }
private:
    struct Slot { double last = 0; bool emitted = false; SlowCallSuppression pending; };
    std::array<Slot, static_cast<std::size_t>(SlowCallPhase::Count)> slots_{};
};
}
