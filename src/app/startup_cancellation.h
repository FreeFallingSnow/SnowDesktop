#pragma once

#include <windows.h>
#include <atomic>

namespace snowdesktop
{
// Private startup ownership gate. Cancellation and desktop takeover cannot both
// win: a canceled process never gets as far as hiding Explorer's icon layer.
class StartupCancellation final
{
public:
    bool BeginDesktopHandoff() noexcept
    {
        State expected = State::Starting;
        return state_.compare_exchange_strong(expected, State::HandingOff) ||
            expected == State::HandingOff;
    }

    bool IsStarting() const noexcept { return state_.load() == State::Starting; }

    bool TerminateStartup() noexcept
    {
        State expected = State::Starting;
        if (!state_.compare_exchange_strong(expected, State::Canceled)) return false;
        // Deliberate cancellation is not an exception/crash exit code, so the
        // crash watchdog exits too instead of launching the application again.
        return TerminateProcess(GetCurrentProcess(), ERROR_CANCELLED) != FALSE;
    }

private:
    enum class State { Starting, HandingOff, Canceled };
    std::atomic<State> state_{ State::Starting };
};
}
