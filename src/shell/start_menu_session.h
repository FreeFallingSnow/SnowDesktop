#pragma once

#include <atomic>
#include <cstdint>

namespace snowdesktop::start_menu
{
enum class Dismissal { Closed, Cancelled, NotFound, ProtectionLost };

// The opening down can reach the global sampler after the Dock handler.
// It belongs to the open action, not an immediate outside-click dismissal.
class PointerDismissal final
{
public:
    void Reset() noexcept { released_ = false; }
    bool Observe(bool down, bool pressed, bool ownedSurface) noexcept
    {
        if (!released_) { released_ = !down; return false; }
        return pressed && ownedSurface;
    }
private:
    bool released_ = false;
};

class CloseRequest final
{
public:
    void Reset() noexcept { requested_.store(0); sent_ = false; }
    void Request(std::uint64_t now) noexcept
    {
        std::uint64_t empty = 0;
        requested_.compare_exchange_strong(empty, now + 1);
    }
    bool Requested() const noexcept { return requested_.load() != 0; }
    bool Take(std::uint64_t now, bool visible, bool foreground) noexcept
    {
        const auto request = requested_.load();
        if (!request || sent_ || !visible) return false;
        // A normal activating click can already be dismissing native Start.
        // Allow it to settle, but do not abandon a nonactivating Dock/bar click.
        if (!foreground && now - (request - 1) < 150) return false;
        sent_ = true;
        return true;
    }
    bool Expired(std::uint64_t now) const noexcept
    {
        const auto request = requested_.load();
        return request && now - (request - 1) >= 1500;
    }
private:
    std::atomic<std::uint64_t> requested_{0};
    bool sent_ = false; // Worker only; Reset runs before the worker starts.
};

// Keep the taskbar lease alive until the native Start menu is dismissed.
// Observe/renew/wait/clock are the external Windows boundaries of this loop.
template<class Observe, class Renew, class Wait, class Clock>
Dismissal WaitForDismissal(bool seen, Observe observe, Renew renew, Wait wait, Clock clock)
{
    const auto started = clock();
    auto renewed = started;
    unsigned hidden = 0;
    if (!renew()) return Dismissal::ProtectionLost;
    while (wait(50))
    {
        const auto now = clock();
        if (observe()) { seen = true; hidden = 0; }
        else if (seen && ++hidden >= 2) return Dismissal::Closed;
        else if (!seen && now - started >= 2000) return Dismissal::NotFound;
        if (now - renewed >= 500)
        {
            if (!renew()) return Dismissal::ProtectionLost;
            renewed = now;
        }
    }
    return Dismissal::Cancelled;
}

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
