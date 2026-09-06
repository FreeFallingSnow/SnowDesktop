#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

// Internal instrumentation, deliberately independent of the capture backend.
// Without an explicitly started session, hooks stay null: no clocks, locks,
// strings, buffers, threads, timers or file I/O are used by these probes.
namespace snowdesktop::performance
{
struct ScopeToken
{
    std::uint64_t generation = 0, id = 0, parent = 0, correlation = 0;
    std::uint64_t start = 0, cpuStart = 0, childWall = 0, childCpu = 0;
    unsigned long thread = 0;
    bool cpuAvailable = false;
    ScopeToken* previous = nullptr;
    std::string module, phase, owner;
};

struct Hooks
{
    void (*begin)(ScopeToken&, std::string_view, std::string_view,
        std::wstring_view, std::uint64_t) noexcept;
    void (*end)(ScopeToken&) noexcept;
    void (*value)(std::string_view, std::string_view, std::wstring_view,
        double, std::uint64_t) noexcept;
    void (*drawLink)(std::string_view, std::wstring_view, bool) noexcept;
};
inline std::atomic<const Hooks*> captureHooks{ nullptr };

inline bool Enabled() noexcept
{
    return captureHooks.load(std::memory_order_relaxed) != nullptr;
}

class Scope final
{
public:
    Scope(std::string_view module, std::string_view phase,
        std::wstring_view owner = {}, std::uint64_t correlation = 0) noexcept
        : hooks_(captureHooks.load(std::memory_order_acquire))
    {
        if (hooks_) hooks_->begin(token_, module, phase, owner, correlation);
    }
    ~Scope() { if (hooks_) hooks_->end(token_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const Hooks* hooks_;
    ScopeToken token_;
};

// Values are gauges/counters, never CPU durations. correlation connects task
// lifecycle events; synchronous parent IDs connect invalidations to callbacks.
inline void Value(std::string_view module, std::string_view name,
    std::wstring_view owner, double value, std::uint64_t correlation = 0) noexcept
{
    if (const auto* hooks = captureHooks.load(std::memory_order_acquire))
        hooks->value(module, name, owner, value, correlation);
}

inline void DrawLink(std::string_view surface, std::wstring_view owner,
    bool consume) noexcept
{
    if (const auto* hooks = captureHooks.load(std::memory_order_acquire))
        hooks->drawLink(surface, owner, consume);
}
}
