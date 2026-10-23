#pragma once

#include "start_menu_position.h"
#include <cstdint>
#include <string>

namespace snowdesktop::start_menu
{
inline constexpr wchar_t kMessage[] = L"SnowDesktop.StartMenu.Place.v1";
inline constexpr DWORD kMagic = 0x5344534d;
enum Status : LONG { Pending, Prepared, Positioned, Closed, Unsupported };
struct Request
{
    DWORD magic = kMagic, version = 1, bytes = sizeof(Request), owner = 0, target = 0;
    std::uint64_t token = 0;
    std::uintptr_t ownerWindow = 0;
    Placement placement;
    alignas(8) volatile LONG64 heartbeat = 0;
    volatile LONG active = TRUE;
    volatile LONG status = Pending;
    volatile LONG error = ERROR_SUCCESS;
};
inline std::wstring MappingName(DWORD owner, std::uint64_t token)
{
    return L"Local\\SnowDesktop.StartMenu." + std::to_wstring(owner) + L"." + std::to_wstring(token);
}
inline bool ValidRequest(const Request& r, DWORD owner, DWORD target, std::uint64_t token) noexcept
{
    return r.magic == kMagic && r.version == 1 && r.bytes == sizeof(Request) &&
        owner != 0 && r.owner == owner && target != 0 && r.target == target &&
        token != 0 && r.token == token && r.ownerWindow != 0 &&
        Position(r.placement, SIZE{1, 1}).has_value();
}
// Invoked only on the destination's UI thread by the existing Shell-command
// Hook entry. No new public export or third-party integration contract.
void Receive(HWND window, DWORD owner, std::uint64_t token) noexcept;
}
