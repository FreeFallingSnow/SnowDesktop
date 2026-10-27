#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

namespace snowdesktop::start_menu
{
enum class Dismissal { Closed, Cancelled, NotFound, ProtectionLost };

constexpr bool ShouldDispatchInitial(bool existing, bool closeRequested) noexcept
{
    // Cancelling an opening must still close a menu that already existed.
    return existing || !closeRequested;
}

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
    void Reset() noexcept { requested_.store(0); dispatched_.store(0); sent_ = false; }
    void Request(std::uint64_t now) noexcept
    {
        std::uint64_t empty = 0;
        requested_.compare_exchange_strong(empty, now + 1);
    }
    bool Requested() const noexcept { return requested_.load() != 0; }
    void Dispatched(std::uint64_t now) noexcept
    {
        sent_ = true;
        std::uint64_t empty = 0;
        dispatched_.compare_exchange_strong(empty, now + 1);
    }
    bool Take(bool visible) noexcept
    {
        const auto request = requested_.load();
        // This dispatches a one-way Shell dismissal, never a toggle. Start can
        // be visible even when its HWND's process does not own foreground.
        if (!request || sent_ || !visible) return false;
        sent_ = true;
        return true;
    }
    bool Expired(std::uint64_t now) const noexcept
    {
        const auto request = requested_.load();
        const auto dispatch = dispatched_.load();
        const auto start = dispatch ? dispatch : request;
        return request && now - (start - 1) >= 1500;
    }
private:
    std::atomic<std::uint64_t> requested_{0};
    std::atomic<std::uint64_t> dispatched_{0};
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
        continuation_ = {};
        return true;
    }
    void AbortBegin() noexcept { phase_.store(Phase::Idle); }
    void Cancel() noexcept { cancelled_.store(true); }
    // UI only. A replacement surface must wait for native dismissal and Dock
    // cleanup, otherwise its first focus request competes with Start.
    bool Defer(std::function<void()> action)
    {
        if (!Pending()) return false;
        continuation_ = std::move(action);
        Cancel();
        return true;
    }
    void DiscardContinuation() noexcept { continuation_ = {}; }
    bool Pending() const noexcept { return phase_.load() != Phase::Idle; }
    void Complete(bool fallback, bool dismissed = true) noexcept
    {
        dismissed_.store(dismissed);
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
        catch (...) { continuation_ = {}; phase_.store(Phase::Idle); throw; }
        auto next = std::move(continuation_);
        const bool dismissed = dismissed_.load();
        phase_.store(Phase::Idle);
        if (dismissed && next) next();
        return true;
    }
private:
    enum class Phase { Idle, Running, Completed, Fallback, Finishing };
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic_bool cancelled_{false};
    std::atomic_bool dismissed_{false};
    std::function<void()> continuation_; // UI only, including Finish.
};
}
