#pragma once

#include <windows.h>
#include <dxgiformat.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace snow::wallpaper_hook {

constexpr std::uint32_t kMagic = 0x534E4F57; // SNOW
constexpr std::uint32_t kVersion = 2;
constexpr std::size_t kMaxFrameSlots = 8;

enum class Status : LONG {
    starting = 1,
    hooked = 2,
    sharing = 3,
    stopping = 4,
    failed = -1,
};

struct SharedFrameSlot {
    std::uint64_t swap_chain;
    std::uint64_t output_window;
    std::uint64_t shared_handle;
    volatile LONG generation;
    volatile LONG completed_request_serial;
    volatile LONG64 frame_number;
    volatile LONG64 consumed_frame_number;
    volatile LONG64 skipped_frames;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint32_t adapter_luid_low;
    std::int32_t adapter_luid_high;
    RECT desktop_rect;
    wchar_t window_class[64];
};

struct SharedState {
    std::uint32_t magic;
    std::uint32_t version;
    DWORD process_id;
    volatile LONG status;
    volatile LONG last_hresult;
    volatile LONG shutdown_requested;
    volatile LONG active_hooks;
    volatile LONG capture_enabled;
    volatile LONG requested_interval_ms;
    volatile LONG request_serial;
    volatile LONG slot_count;
    volatile LONG64 producer_heartbeat;
    volatile LONG64 consumer_heartbeat;
    SharedFrameSlot slots[kMaxFrameSlots];
};

inline void MakeMappingName(DWORD processId, wchar_t (&buffer)[128])
{
    _snwprintf_s(buffer, 128, _TRUNCATE,
        L"Local\\SnowDesktop.WallpaperEngine.DxgiHook.v2.%lu",
        static_cast<unsigned long>(processId));
}

} // namespace snow::wallpaper_hook
