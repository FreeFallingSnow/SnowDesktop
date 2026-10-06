#pragma once

#include <windows.h>

#include <cstdint>
#include <limits>
#include <string_view>

namespace snowdesktop::application_restart_policy
{
inline constexpr DWORD kFlags = RESTART_NO_HANG;
inline constexpr std::wstring_view kWatchProcessHandlePrefix =
    L"--watch-process-handle=";

constexpr bool AllowsCrashRestart(DWORD flags)
{
    return (flags & RESTART_NO_CRASH) == 0;
}

constexpr bool AllowsHangRestart(DWORD flags)
{
    return (flags & RESTART_NO_HANG) == 0;
}

constexpr bool IsCrashExitCode(DWORD exitCode)
{
    return (exitCode & 0xC0000000u) == 0xC0000000u;
}

constexpr std::uintptr_t ParseWatchProcessHandle(
    std::wstring_view commandLine)
{
    const size_t position =
        commandLine.find(kWatchProcessHandlePrefix);
    if (position == std::wstring_view::npos ||
        (position != 0 && commandLine[position - 1] != L' ' &&
            commandLine[position - 1] != L'\t'))
    {
        return 0;
    }

    size_t cursor = position + kWatchProcessHandlePrefix.size();
    if (cursor == commandLine.size())
        return 0;
    std::uintptr_t value = 0;
    for (; cursor < commandLine.size(); ++cursor)
    {
        const wchar_t character = commandLine[cursor];
        if (character == L' ' || character == L'\t')
            break;
        if (character < L'0' || character > L'9')
            return 0;
        const auto digit = static_cast<std::uintptr_t>(
            character - L'0');
        if (value >
            (std::numeric_limits<std::uintptr_t>::max() - digit) / 10)
        {
            return 0;
        }
        value = value * 10 + digit;
    }
    return value;
}

static_assert(AllowsCrashRestart(kFlags));
static_assert(!AllowsHangRestart(kFlags));
static_assert(IsCrashExitCode(0xC0000409u));
static_assert(!IsCrashExitCode(ERROR_SUCCESS));
}
