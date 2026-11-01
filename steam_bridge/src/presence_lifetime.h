// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string_view>

namespace snowdesktop::steam_bridge
{
// Handles are explicitly inherited by the host, never discovered by PID/name.
template<class Tick>
int RunPresenceLifetime(int argc, wchar_t** argv, Tick tick)
{
    if (argc != 6 || std::wstring_view(argv[2]) != L"--parent-handle" ||
        std::wstring_view(argv[4]) != L"--stop-handle") return 64;
    const auto parse = [](const wchar_t* text) -> HANDLE {
        if (!text || !*text || std::wstring_view(text).find_first_not_of(L"0123456789") != std::wstring_view::npos) return nullptr;
        wchar_t* end = nullptr;
        const auto value = std::wcstoull(text, &end, 10);
        if (!value || *end || value >= (std::numeric_limits<std::uintptr_t>::max)()) return nullptr;
        const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
        DWORD flags = 0;
        return GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT) ? handle : nullptr;
    };
    HANDLE waits[]{parse(argv[2 + 1]), parse(argv[4 + 1])};
    if (!waits[0] || !waits[1] || waits[0] == waits[1]) return 64;
    DWORD result = WAIT_TIMEOUT;
    while ((result = WaitForMultipleObjects(2, waits, FALSE, 250)) == WAIT_TIMEOUT) tick();
    CloseHandle(waits[0]);
    CloseHandle(waits[1]);
    return result == WAIT_FAILED ? 5 : 0;
}
}
