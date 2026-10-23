#pragma once

#include <atomic>

namespace snowdesktop::start_menu
{
// The worker completing is only half of an invocation. Keep it reserved until
// the UI has released its Dock hold, including while a fallback is dispatched.
class Invocation final
{
public:
    bool Begin() noexcept
    {
        auto expected = Phase::Idle;
        if (!phase_.compare_exchange_strong(expected, Phase::Running)) return false;
        cancelled_.store(false);
        return true;
    }
    void AbortBegin() noexcept { phase_.store(Phase::Idle); }
    void Cancel() noexcept { cancelled_.store(true); }
    bool Pending() const noexcept { return phase_.load() != Phase::Idle; }
    void Complete(bool fallback) noexcept
    {
        phase_.store(fallback ? Phase::Fallback : Phase::Completed);
    }
    template<class ReleaseHold, class NativeFallback>
    bool Finish(ReleaseHold releaseHold, NativeFallback nativeFallback)
    {
        auto completed = phase_.load();
        if (completed != Phase::Completed && completed != Phase::Fallback) return false;
        if (!phase_.compare_exchange_strong(completed, Phase::Finishing)) return false;
        try
        {
            releaseHold();
            // Re-entrant cancellation during UI cleanup must suppress input.
            if (completed == Phase::Fallback && !cancelled_.load()) nativeFallback();
        }
        catch (...) { phase_.store(Phase::Idle); throw; }
        phase_.store(Phase::Idle);
        return true;
    }
private:
    enum class Phase { Idle, Running, Completed, Fallback, Finishing };
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic_bool cancelled_{false};
};
}
