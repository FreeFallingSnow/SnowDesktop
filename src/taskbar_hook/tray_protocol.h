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
inline constexpr DWORD kMagic = 0x53445452, kVersion = 2;
inline constexpr std::size_t kCapacity = 128, kGeometries = 512, kIconSize = 64;
inline constexpr wchar_t kAttachMessage[] = L"SnowDesktop.Tray.Attach.v2";
inline constexpr wchar_t kDetachMessage[] = L"SnowDesktop.Tray.Detach.v2";
inline std::wstring ObjectName(DWORD owner, const wchar_t* suffix)
{ return L"Local\\SnowDesktop.Tray.v2." + std::to_wstring(owner) + L"." + suffix; }

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
    std::uint64_t focusSerial = 0, focusForeground = 0;
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
struct FocusTicket
{
    std::uint64_t epoch = 0, serial = 0, origin = 0, source = 0;
    Identity identity;
    DWORD started = 0, keyboard = 0;
};
inline bool SameFocusIdentity(const Identity& a, const Identity& b)
{ return SameIdentity(a, b) && a.window == b.window && a.id == b.id && a.process == b.process; }
inline bool MatchesFocusRequest(const Identity& registered, const Identity& request)
{
    if (!HasGuid(request.guid)) return SameFocusIdentity(registered, request);
    // NIF_GUID callers may specify only their GUID. A supplied HWND must still
    // belong to the registered incarnation of that application.
    return registered.guid == request.guid && (!request.window ||
        (registered.window == request.window && registered.process == request.process));
}
inline bool FocusForegroundAllowed(const FocusTicket& ticket, std::uint64_t window, DWORD process)
{
    return window && (window == ticket.origin || window == ticket.source ||
        (process && process == ticket.identity.process));
}
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
    volatile LONG focusSequence = 0;
    alignas(8) volatile LONG64 focusClaimed = 0;
    FocusTicket focus;
    Event events[kCapacity]{};
    volatile LONG received = 0, decoded = 0, rejected = 0, lastSize = 0;
};
inline LONG Read(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
inline LONG64 Read(volatile LONG64& value) { return InterlockedCompareExchange64(&value, 0, 0); }
inline void WriteFocusTicket(SharedState& state, const FocusTicket& ticket)
{
    InterlockedIncrement(&state.focusSequence);
    state.focus = ticket;
    MemoryBarrier(); InterlockedIncrement(&state.focusSequence);
}
inline bool ReadFocusTicket(SharedState& state, const Identity& identity, FocusTicket& result)
{
    const LONG before = Read(state.focusSequence);
    if (before & 1) return false;
    const auto ticket = state.focus;
    MemoryBarrier();
    if (before != Read(state.focusSequence) || !ticket.serial || !ticket.origin ||
        ticket.epoch != static_cast<std::uint64_t>(Read(state.epoch)) ||
        !MatchesFocusRequest(ticket.identity, identity)) return false;
    result = ticket;
    return true;
}
inline bool ClaimFocusTicket(SharedState& state, const FocusTicket& ticket)
{
    const auto previous = Read(state.focusClaimed);
    return previous < static_cast<LONG64>(ticket.serial) &&
        InterlockedCompareExchange64(&state.focusClaimed, static_cast<LONG64>(ticket.serial), previous) == previous;
}
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
// Modern Explorer packets can append opaque data (1484 bytes observed on
// Windows 11). Decode only the known prefix, with a bounded envelope; the
// trailer is neither copied on Explorer's UI thread nor interpreted here.
inline constexpr std::size_t kMaxNotificationBytes = 8192;

inline bool Decode(const void* bytes, std::size_t size, Notification& output, HICON& icon)
{
    constexpr auto minimum = offsetof(ShellTrayData, icon) + offsetof(NotifyIcon32, tip);
    if (!bytes || size < minimum || size > kMaxNotificationBytes) return false;
    ShellTrayData wire{};
    std::memcpy(&wire, bytes, (std::min)(size, sizeof(wire)));
    if (wire.operation != NIM_ADD && wire.operation != NIM_MODIFY &&
        wire.operation != NIM_DELETE && wire.operation != NIM_SETVERSION && wire.operation != NIM_SETFOCUS) return false;
    const auto& data = wire.icon;
    if (data.size < offsetof(NotifyIcon32, tip) || data.size > sizeof(NOTIFYICONDATAW)) return false;
    // The header may retain the caller's native cbSize while the payload uses
    // 32-bit handles. Bounds come from COPYDATA, never from that native size.
    const auto available = (std::min)({static_cast<std::size_t>(data.size),
        size - offsetof(ShellTrayData, icon), sizeof(NotifyIcon32)});
    const auto contains = [&](std::size_t end) { return available >= end; };
    if ((data.flags & NIF_GUID) && !contains(offsetof(NotifyIcon32, guid) + sizeof(GUID))) return false;
    if (wire.operation != NIM_SETFOCUS && (data.flags & NIF_STATE) && !contains(offsetof(NotifyIcon32, stateMask) + sizeof(DWORD))) return false;
    if (wire.operation == NIM_SETVERSION && !contains(offsetof(NotifyIcon32, version) + sizeof(DWORD))) return false;
    output = {};
    output.operation = wire.operation; output.flags = data.flags;
    output.identity.window = data.window; output.identity.id = data.id;
    if (data.flags & NIF_GUID) output.identity.guid = data.guid;
    // This operation only identifies an icon; ignore stale image/state bits.
    if (wire.operation == NIM_SETFOCUS) { output.flags &= NIF_GUID; icon = nullptr; return true; }
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
inline LRESULT GeometryReply(DWORD message, const RECT& rect)
{
    // Shell_NotifyIconGetRect asks for origin, then extent, not two corners.
    return message == 1 ? MAKELONG(rect.left, rect.top) :
        MAKELONG(rect.right - rect.left, rect.bottom - rect.top);
}
inline Callback MakeCallback(DWORD version, DWORD id, UINT message, POINT anchor)
{
    if (version >= NOTIFYICON_VERSION_4)
        return {static_cast<WPARAM>(MAKELONG(anchor.x, anchor.y)),
            static_cast<LPARAM>(MAKELONG(message, id))};
    return {id, message};
}
}
