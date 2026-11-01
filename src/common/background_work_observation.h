#pragma once

#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>

// Optional value-only diagnostics for BackgroundWork. The log never owns the
// state, request, completion or window; a full log is not worker quiescence.
namespace snowdesktop::background_work_observation
{
enum class Phase : unsigned { coalesced, queue_full, queued, worker_enter, worker_return,
    ready, post_result, discarded, apply_enter, apply_return };
struct Event
{
    Phase phase{};
    std::uintptr_t state = 0, context = 0, window = 0;
    std::uint64_t serial = 0;
    DWORD thread = 0;
    UINT message = 0;
    bool result = false;
};
inline constexpr std::size_t capacity = 8192;
inline constexpr std::uint32_t active = std::uint32_t{1} << 31;
inline constexpr std::uint32_t stoppedBit = std::uint32_t{1} << 30;
inline constexpr std::uint32_t configuringBit = std::uint32_t{1} << 29;
struct Slot { Event event{}; std::atomic_bool ready{false}; };
inline std::array<Slot, capacity> slots;
struct Storage final { Slot* slots = nullptr; std::size_t capacity = 0, committedBytes = 0; };
inline Storage defaultStorage{slots.data(), capacity, 0}, configuredStorage{};
inline std::atomic<Storage*> storage{&defaultStorage};
inline std::atomic<std::uint32_t> gate{0};
inline std::atomic<std::size_t> next{0};
inline std::atomic_bool claimed{false}, started{false}, lost{false}, stopped{false}, capacityExceeded{false};
inline std::atomic_bool configurationAttempted{false}, configurationFailed{false};
struct CaptureConfiguration final { bool configured = false, resourceUnavailable = false; std::size_t committedBytes = 0; };
// Single opt-in allocation before capture, retained until process exit. This
// only bounds this recorder's VM allocation; it does not certify isolation,
// other allocations, worker quiescence, or the size of copied evidence.
inline CaptureConfiguration ConfigureBudgetOnce(std::size_t eventCapacity, std::size_t byteBudget) noexcept
{
    CaptureConfiguration out;
    const DWORD saved = GetLastError();
    struct ErrorGuard { DWORD value; ~ErrorGuard() { SetLastError(value); } } restore{saved};
    std::uint32_t idle = 0;
    if (claimed.load(std::memory_order_acquire) || !gate.compare_exchange_strong(idle, configuringBit, std::memory_order_acq_rel)) return out;
    struct GateGuard { ~GateGuard() { gate.fetch_and(~configuringBit, std::memory_order_release); } } guard;
    bool first = false;
    if (!configurationAttempted.compare_exchange_strong(first, true, std::memory_order_acq_rel)) return out;
    const auto fail = [&]() noexcept { configurationFailed.store(true, std::memory_order_release);
        gate.fetch_or(stoppedBit, std::memory_order_acq_rel); out.resourceUnavailable = true; };
    if (claimed.load(std::memory_order_acquire) || !eventCapacity || !byteBudget ||
        eventCapacity > (std::numeric_limits<std::size_t>::max)() / sizeof(Slot)) { fail(); return out; }
    const auto used = eventCapacity * sizeof(Slot);
    SYSTEM_INFO info{}; GetSystemInfo(&info); const auto page = static_cast<std::size_t>(info.dwPageSize);
    if (!page || used > (std::numeric_limits<std::size_t>::max)() - (page - 1)) { fail(); return out; }
    const auto committed = ((used + page - 1) / page) * page;
    if (!committed || committed > byteBudget) { fail(); return out; }
    auto* block = static_cast<Slot*>(VirtualAlloc(nullptr, committed, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block) { fail(); return out; }
    static_assert(std::is_nothrow_default_constructible_v<Slot> && std::is_trivially_destructible_v<Slot>);
    for (std::size_t i = 0; i < eventCapacity; ++i) new (block + i) Slot{};
    if (claimed.load(std::memory_order_acquire) || gate.load(std::memory_order_acquire) != configuringBit)
    { VirtualFree(block, 0, MEM_RELEASE); fail(); return out; }
    configuredStorage = {block, eventCapacity, committed};
    storage.store(&configuredStorage, std::memory_order_release);
    out.configured = true; out.committedBytes = committed; return out;
}
inline bool Enable() noexcept
{
    if (!gate.is_lock_free() || !next.is_lock_free() || !claimed.is_lock_free() || !started.is_lock_free() ||
        !stopped.is_lock_free() || !lost.is_lock_free() || !capacityExceeded.is_lock_free() ||
        !configurationAttempted.is_lock_free() || !configurationFailed.is_lock_free() || !storage.is_lock_free() ||
        !slots.front().ready.is_lock_free()) return false;
    bool expected = false;
    if (!claimed.compare_exchange_strong(expected, true)) return false;
    if (configurationFailed.load(std::memory_order_acquire)) return false;
    std::uint32_t idle = 0;
    if (!gate.compare_exchange_strong(idle, active, std::memory_order_release, std::memory_order_relaxed)) return false;
    started.store(true, std::memory_order_release);
    return true;
}
inline void Stop() noexcept
{
    if (!claimed.load(std::memory_order_acquire)) return;
    auto value = gate.load(std::memory_order_acquire);
    while (!(value & stoppedBit) && !gate.compare_exchange_weak(value, (value & ~active) | stoppedBit,
        std::memory_order_acq_rel, std::memory_order_acquire)) {}
    stopped.store(true, std::memory_order_release);
}
inline bool Complete() noexcept
{ return started.load(std::memory_order_acquire) && stopped.load(std::memory_order_acquire) &&
    gate.load(std::memory_order_acquire) == stoppedBit && !lost.load(std::memory_order_acquire); }
inline std::size_t Count() noexcept { return next.load(std::memory_order_acquire); }
struct BudgetObservation
{
    std::size_t eventCapacity = capacity, recordedEvents = 0, committedBytes = 0;
    bool captureClaimed = false, writersDrained = false, recordsLost = false, capacityExceeded = false;
};
// An observation of this independent log only. It neither owns nor waits for
// BackgroundWork, and values read while writers run are not one atomic snapshot.
inline BudgetObservation ObserveBudget() noexcept
{
    BudgetObservation out;
    const auto* current = storage.load(std::memory_order_acquire);
    out.eventCapacity = current->capacity; out.committedBytes = current->committedBytes;
    out.captureClaimed = claimed.load(std::memory_order_acquire);
    out.writersDrained = started.load(std::memory_order_acquire) && stopped.load(std::memory_order_acquire) && gate.load(std::memory_order_acquire) == stoppedBit;
    out.recordedEvents = next.load(std::memory_order_acquire);
    out.recordsLost = lost.load(std::memory_order_acquire);
    out.capacityExceeded = capacityExceeded.load(std::memory_order_acquire);
    return out;
}
inline bool Read(std::size_t index, Event& event) noexcept
{
    const auto* current = storage.load(std::memory_order_acquire);
    if (!Complete() || index >= Count() || index >= current->capacity || !current->slots[index].ready.load(std::memory_order_acquire)) return false;
    event = current->slots[index].event;
    return true;
}
inline void Record(Phase phase, const void* state, std::uintptr_t context, std::uint64_t serial,
    HWND window = nullptr, UINT message = 0, bool result = false) noexcept
{
    if (!context) return;
    auto value = gate.load(std::memory_order_acquire);
    while (value & active)
    {
        if ((value & (configuringBit - 1)) == configuringBit - 1) { capacityExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); return; }
        if (gate.compare_exchange_weak(value, value + 1, std::memory_order_acquire, std::memory_order_relaxed))
        {
            const DWORD error = GetLastError();
            const auto* current = storage.load(std::memory_order_acquire);
            auto index = next.load(std::memory_order_relaxed);
            while (index < current->capacity && !next.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {}
            if (index >= current->capacity) { capacityExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); }
            else
            {
                current->slots[index].event = {phase, reinterpret_cast<std::uintptr_t>(state), context,
                    reinterpret_cast<std::uintptr_t>(window), serial, GetCurrentThreadId(), message, result};
                current->slots[index].ready.store(true, std::memory_order_release);
            }
            SetLastError(error);
            gate.fetch_sub(1, std::memory_order_release);
            return;
        }
    }
}
}
