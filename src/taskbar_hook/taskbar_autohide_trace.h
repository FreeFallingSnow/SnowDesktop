#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <algorithm>

// Private host/Explorer diagnostic transport. It never controls activation or
// visibility. A busy/full buffer drops records instead of blocking either UI.
namespace snowdesktop::taskbar_hook
{
enum class AutoHideTraceKind : LONG
{
    Adapter = 0, ActivateBefore, ActivateAfter, PrimaryUnhide, SecondaryUnhide,
    SuppressedActivation, FocusEnter, FocusLeave
};

struct AutoHideTraceRecord
{
    ULONGLONG tick = 0;
    AutoHideTraceKind kind{};
    DWORD threadId = 0;
    std::uintptr_t taskbar = 0, previous = 0, foreground = 0;
    RECT rect{};
    POINT cursor{};
    LONG activation = 0, previousIconic = 0, geometryValid = 0;
    LONG flags = 0, request = 0;
    LONG explicitFocus = 0;
    std::uint32_t callerRva = 0;
};

inline constexpr LONG kAutoHideTraceCapacity = 64;
struct AutoHideTraceBuffer
{
    volatile LONG lock = 0, count = 0, dropped = 0;
    AutoHideTraceRecord records[kAutoHideTraceCapacity]{};
};

inline bool AppendAutoHideTrace(AutoHideTraceBuffer& buffer,
    const AutoHideTraceRecord& record) noexcept
{
    if (InterlockedCompareExchange(&buffer.lock, 1, 0) != 0)
    {
        InterlockedIncrement(&buffer.dropped);
        return false;
    }
    const LONG count = buffer.count;
    const bool available = count >= 0 && count < kAutoHideTraceCapacity;
    if (available)
    {
        buffer.records[count] = record;
        buffer.count = count + 1;
    }
    else InterlockedIncrement(&buffer.dropped);
    InterlockedExchange(&buffer.lock, 0);
    return available;
}

inline LONG DrainAutoHideTrace(AutoHideTraceBuffer& buffer,
    std::array<AutoHideTraceRecord, kAutoHideTraceCapacity>& records,
    LONG& dropped) noexcept
{
    dropped = 0;
    if (InterlockedCompareExchange(&buffer.lock, 1, 0) != 0) return 0;
    const LONG available = buffer.count;
    const LONG count = std::clamp<LONG>(available, 0, kAutoHideTraceCapacity);
    std::copy_n(buffer.records, count, records.begin());
    buffer.count = 0;
    dropped = InterlockedExchange(&buffer.dropped, 0);
    InterlockedExchange(&buffer.lock, 0);
    return count;
}
}
