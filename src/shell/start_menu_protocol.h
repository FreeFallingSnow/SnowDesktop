#pragma once

#include "start_menu_position.h"
#include <cstdint>
#include <string>

namespace snowdesktop::start_menu
{
inline constexpr wchar_t kMessage[] = L"SnowDesktop.StartMenu.Place.v3";
inline constexpr DWORD kMagic = 0x5344534d;
inline constexpr DWORD kVersion = 3;
enum Status : LONG { Pending, Prepared, Positioned, Closed, Unsupported };
enum Stage : LONG { WaitingForWindow, Attached, ReadingRoot, WaitingForLayout, MovingHost, MovingFrame, PlacementApplied };
struct Request
{
    DWORD magic = kMagic, version = kVersion, bytes = sizeof(Request), owner = 0, target = 0;
    std::uint64_t token = 0;
    std::uintptr_t ownerWindow = 0;
    Placement placement;
    alignas(8) volatile LONG64 heartbeat = 0;
    volatile LONG active = TRUE;
    volatile LONG status = Pending;
    volatile LONG error = ERROR_SUCCESS;
    volatile LONG stage = WaitingForWindow, ticks = 0;
    wchar_t rootClass[96]{}, frameClass[96]{};
    SIZE measuredSize{};
    volatile LONG boundsGeneration = 0;
    RECT menuBounds{};
};
inline std::wstring MappingName(DWORD owner, std::uint64_t token)
{
    return L"Local\\SnowDesktop.StartMenu.v3." + std::to_wstring(owner) + L"." + std::to_wstring(token);
}
inline bool ValidRequest(const Request& r, DWORD owner, DWORD target, std::uint64_t token) noexcept
{
    return r.magic == kMagic && r.version == kVersion && r.bytes == sizeof(Request) &&
        owner != 0 && r.owner == owner && target != 0 && r.target == target &&
        token != 0 && r.token == token && r.ownerWindow != 0 &&
        Position(r.placement, SIZE{1, 1}).has_value();
}

inline void PublishBounds(Request& request, const RECT& bounds) noexcept
{
    InterlockedIncrement(&request.boundsGeneration);
    request.menuBounds = bounds;
    MemoryBarrier();
    InterlockedIncrement(&request.boundsGeneration);
}

inline RECT ReadBounds(Request& request) noexcept
{
    for (unsigned attempt = 0; attempt != 3; ++attempt)
    {
        const LONG before = InterlockedCompareExchange(&request.boundsGeneration, 0, 0);
        if (before & 1) continue;
        const RECT bounds = request.menuBounds;
        MemoryBarrier();
        if (before == InterlockedCompareExchange(&request.boundsGeneration, 0, 0)) return bounds;
    }
    return {};
}

// CoreWindow pumps need not dispatch ordinary WM_TIMER messages. The worker
// explicitly advances the installed receiver without blocking desktop input.
inline DWORD PulsePlacement(HWND window) noexcept
{
    DWORD_PTR ignored = 0;
    SetLastError(ERROR_SUCCESS);
    if (SendMessageTimeoutW(window, RegisterWindowMessageW(kMessage), 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &ignored)) return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return error ? error : ERROR_TIMEOUT;
}
// Invoked only on the destination's UI thread by the existing Shell-command
// Hook entry. No new public export or third-party integration contract.
void Receive(HWND window, DWORD owner, std::uint64_t token) noexcept;
}
