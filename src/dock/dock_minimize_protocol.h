#pragma once

#include <windows.h>

namespace snowdesktop::dock_minimize
{
inline constexpr wchar_t kTargetProperty[] = L"SnowDesktop.Dock.MinimizeTarget.v1";
inline constexpr wchar_t kOwnerProperty[] = L"SnowDesktop.Dock.MinimizeOwner.v1";
inline constexpr wchar_t kRevisionProperty[] = L"SnowDesktop.Dock.MinimizeRevision.v1";
inline constexpr wchar_t kReadyProperty[] = L"SnowDesktop.Dock.MinimizeReady.v1";
inline constexpr wchar_t kRequestMessage[] = L"SnowDesktop.Dock.BeforeMinimize.v1";
inline constexpr wchar_t kCancelMessage[] = L"SnowDesktop.Dock.CancelMinimize.v1";
inline constexpr DWORD kRequestTimeoutMs = 180;

inline bool HasLiveOwner(HWND receiver) noexcept
{
    DWORD process = 0;
    return receiver && GetWindowThreadProcessId(receiver, &process) && process &&
        GetPropW(receiver, kOwnerProperty) == ULongToHandle(process) &&
        GetPropW(receiver, kRevisionProperty) != nullptr;
}

// DWORD ticks are shared by x86/x64 and deliberately wrap at 49.7 days.
constexpr bool RequestIsCurrent(DWORD deadline, DWORD now) noexcept
{
    const DWORD remaining = deadline - now;
    return remaining > 0 && remaining <= kRequestTimeoutMs;
}

constexpr bool IsMinimizeCommand(UINT command) noexcept
{
    return command == SW_MINIMIZE || command == SW_SHOWMINIMIZED ||
        command == SW_SHOWMINNOACTIVE || command == SW_FORCEMINIMIZE;
}

inline UINT RequestMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(kRequestMessage);
    return message;
}

inline UINT CancelMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(kCancelMessage);
    return message;
}
}
