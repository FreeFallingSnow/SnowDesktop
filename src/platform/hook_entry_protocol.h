#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <algorithm>

namespace snowdesktop::hook_entry
{
// This is an internal, fixed-width x86/x64 contract. The entry image does not
// link against taskbar/Dock implementation headers or change with their IPC.
inline constexpr DWORD kAbi = 1;
inline constexpr DWORD kMagic = 0x53444845;
inline constexpr wchar_t kOwnerProperty[] = L"SnowDesktop.Hook.Entry.Owner.v1";
inline constexpr std::size_t kPathCapacity = 32768;
enum class Payload : std::uint32_t { Taskbar, Dock64, Dock32, Count };
inline constexpr std::size_t kPayloadCount = static_cast<std::size_t>(Payload::Count);
struct alignas(8) Catalog
{
    DWORD magic = kMagic, version = kAbi, bytes = sizeof(Catalog), owner = 0;
    std::uint64_t ownerCreation = 0;
    volatile LONG sequence = 0;
    DWORD reserved = 0;
    std::array<std::uint64_t, kPayloadCount> generations{};
    std::array<std::array<wchar_t, kPathCapacity>, kPayloadCount> paths{};
};
struct Snapshot
{
    std::uint64_t ownerCreation = 0, generation = 0;
    std::wstring path;
};
inline std::wstring CatalogName(DWORD owner)
{ return L"Local\\SnowDesktop.Hook.Entry.v1." + std::to_wstring(owner); }
inline std::uint64_t ProcessCreation(HANDLE process) noexcept
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    return GetProcessTimes(process, &creation, &exit, &kernel, &user)
        ? static_cast<std::uint64_t>(creation.dwHighDateTime) << 32 | creation.dwLowDateTime : 0;
}
inline bool Read(const Catalog& catalog, DWORD owner, Payload payload, Snapshot& result)
{
    const auto index = static_cast<std::size_t>(payload);
    if (index >= kPayloadCount || catalog.magic != kMagic || catalog.version != kAbi ||
        catalog.bytes != sizeof(Catalog) || !owner || catalog.owner != owner || !catalog.ownerCreation) return false;
    const LONG sequence = catalog.sequence;
    if (sequence & 1) return false;
    MemoryBarrier();
    const auto& path = catalog.paths[index];
    const auto end = std::find(path.begin(), path.end(), L'\0');
    if (end == path.begin() || end == path.end()) return false;
    Snapshot copy{catalog.ownerCreation, catalog.generations[index], std::wstring(path.begin(), end)};
    MemoryBarrier();
    if (!copy.generation || sequence != catalog.sequence) return false;
    result = std::move(copy);
    return true;
}
}
