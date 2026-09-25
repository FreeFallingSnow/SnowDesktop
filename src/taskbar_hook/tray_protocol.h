#pragma once
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace snowdesktop::tray
{
inline constexpr DWORD kMagic = 0x53445452, kVersion = 1;
inline constexpr std::size_t kCapacity = 128, kGeometries = 512, kIconSize = 64;
inline constexpr wchar_t kAttachMessage[] = L"SnowDesktop.Tray.Attach.v1";
inline constexpr wchar_t kDetachMessage[] = L"SnowDesktop.Tray.Detach.v1";
inline std::wstring ObjectName(DWORD owner, const wchar_t* suffix)
{ return L"Local\\SnowDesktop.Tray.v1." + std::to_wstring(owner) + L"." + suffix; }

struct Identity
{
    GUID guid{};
    std::uint64_t window = 0;
    DWORD id = 0, process = 0;
};
inline bool HasGuid(const GUID& guid) { return guid != GUID{}; }
inline bool SameIdentity(const Identity& a, const Identity& b)
{
    if (HasGuid(a.guid) || HasGuid(b.guid)) return a.guid == b.guid;
    return a.window == b.window && a.id == b.id && a.process == b.process;
}
struct Notification
{
    std::uint64_t epoch = 0;
    DWORD operation = 0, flags = 0, callback = 0, state = 0, stateMask = 0, version = 0;
    Identity identity;
    wchar_t tip[128]{};
};
struct Event : Notification
{
    DWORD width = 0, height = 0;
    std::array<std::uint32_t, kIconSize * kIconSize> pixels{}; // premultiplied BGRA
};
struct Geometry { Identity identity; RECT rect{}; };
// A single Explorer worker produces events; one host worker consumes them.
// Geometry has the reverse ownership and a non-blocking seqlock reader.
struct SharedState
{
    DWORD magic = kMagic, version = kVersion, size = sizeof(SharedState), owner = 0;
    std::uint64_t ownerCreation = 0;
    alignas(8) volatile LONG64 epoch = 0;
    volatile LONG write = 0, read = 0, ready = 0, stop = 0, resync = 0;
    DWORD explorer = 0;
    volatile LONG geometrySequence = 0;
    DWORD geometryCount = 0;
    Geometry geometries[kGeometries]{};
    Event events[kCapacity]{};
    volatile LONG received = 0, decoded = 0, rejected = 0, lastSize = 0;
};
inline LONG Read(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
inline LONG64 Read(volatile LONG64& value) { return InterlockedCompareExchange64(&value, 0, 0); }
inline bool Publish(SharedState& state, const Event& event)
{
    const LONG write = Read(state.write);
    if (write < 0 || write >= static_cast<LONG>(kCapacity)) return false;
    const LONG next = (write + 1) % static_cast<LONG>(kCapacity);
    if (next == Read(state.read)) { InterlockedIncrement(&state.resync); return false; }
    state.events[write] = event;
    MemoryBarrier();
    InterlockedExchange(&state.write, next);
    return true;
}
inline bool Consume(SharedState& state, Event& event)
{
    const LONG read = Read(state.read);
    if (read < 0 || read >= static_cast<LONG>(kCapacity)) return false;
    if (read == Read(state.write)) return false;
    event = state.events[read];
    MemoryBarrier();
    InterlockedExchange(&state.read, (read + 1) % static_cast<LONG>(kCapacity));
    return true;
}
inline bool LookupGeometry(SharedState& state, const Identity& identity, RECT& result)
{
    const LONG before = Read(state.geometrySequence);
    if (before & 1) return false; // Never wait on the Explorer window thread.
    const DWORD count = state.geometryCount;
    if (count > kGeometries) return false;
    bool found = false;
    for (DWORD i = 0; i < count; ++i)
        if (SameIdentity(state.geometries[i].identity, identity))
        { result = state.geometries[i].rect; found = true; break; }
    MemoryBarrier();
    return before == Read(state.geometrySequence) && found && !IsRectEmpty(&result);
}

// Explorer's private wire layout, adapted from YASB at the revision recorded in
// third_party/yasb/README.md. MIT notices are retained in that directory.
// This layout is deliberately isolated from both the host model and our IPC.
#pragma pack(push, 1)
struct NotifyIcon32
{
    DWORD size, window, id, flags, callback, icon;
    wchar_t tip[128];
    DWORD state, stateMask;
    wchar_t info[256];
    DWORD version;
    wchar_t title[64];
    DWORD infoFlags;
    GUID guid;
    DWORD balloonIcon;
};
struct ShellTrayData { DWORD signature, operation; NotifyIcon32 icon; };
struct IconIdentifier32
{
    DWORD magic, message, size, padding, window, id;
    GUID guid;
};
#pragma pack(pop)
static_assert(sizeof(NotifyIcon32) == 956);

inline bool Decode(const void* bytes, std::size_t size, Notification& output, HICON& icon)
{
    constexpr auto minimum = offsetof(ShellTrayData, icon) + offsetof(NotifyIcon32, tip);
    if (!bytes || size < minimum || size > sizeof(ShellTrayData)) return false;
    ShellTrayData wire{};
    std::memcpy(&wire, bytes, size);
    if (wire.operation != NIM_ADD && wire.operation != NIM_MODIFY &&
        wire.operation != NIM_DELETE && wire.operation != NIM_SETVERSION) return false;
    const auto& data = wire.icon;
    if (data.size < offsetof(NotifyIcon32, tip) || data.size > sizeof(NOTIFYICONDATAW)) return false;
    // The header may retain the caller's native cbSize while the payload uses
    // 32-bit handles. Bounds come from COPYDATA, never from that native size.
    const auto available = (std::min)(static_cast<std::size_t>(data.size), size - offsetof(ShellTrayData, icon));
    const auto contains = [&](std::size_t end) { return available >= end; };
    if ((data.flags & NIF_GUID) && !contains(offsetof(NotifyIcon32, guid) + sizeof(GUID))) return false;
    if ((data.flags & NIF_STATE) && !contains(offsetof(NotifyIcon32, stateMask) + sizeof(DWORD))) return false;
    if (wire.operation == NIM_SETVERSION && !contains(offsetof(NotifyIcon32, version) + sizeof(DWORD))) return false;
    output = {};
    output.operation = wire.operation; output.flags = data.flags;
    output.identity.window = data.window; output.identity.id = data.id;
    if (data.flags & NIF_GUID) output.identity.guid = data.guid;
    output.callback = data.callback; output.state = data.state;
    output.stateMask = data.stateMask; output.version = data.version;
    if (data.flags & NIF_TIP)
    {
        const auto count = (std::min)(std::size(output.tip) - 1,
            (available - offsetof(NotifyIcon32, tip)) / sizeof(wchar_t));
        std::copy_n(data.tip, count, output.tip);
    }
    icon = data.flags & NIF_ICON ? reinterpret_cast<HICON>(static_cast<ULONG_PTR>(data.icon)) : nullptr;
    return true;
}
struct Callback { WPARAM wp = 0; LPARAM lp = 0; };
inline Callback MakeCallback(DWORD version, DWORD id, UINT message, POINT anchor)
{
    if (version >= NOTIFYICON_VERSION_4)
        return {static_cast<WPARAM>(MAKELONG(anchor.x, anchor.y)),
            static_cast<LPARAM>(MAKELONG(message, id))};
    return {id, message};
}
}
