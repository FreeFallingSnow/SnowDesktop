#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace snowdesktop::dock_source_cloak
{
inline constexpr wchar_t OwnerProperty[] = L"SnowDesktop.Dock.SourceCloak.Owner.v1";
inline constexpr wchar_t ProcessProperty[] = L"SnowDesktop.Dock.SourceCloak.Process.v1";
inline constexpr wchar_t TokenProperty[] = L"SnowDesktop.Dock.SourceCloak.Token.v1";
inline constexpr wchar_t HostTokenProperty[] = L"SnowDesktop.Dock.SourceCloak.LeaseHost.v1";
inline constexpr wchar_t CommandName[] = L"SnowDesktop.Dock.SourceCloak.Command.v1";
inline constexpr wchar_t ResultProperty[] = L"SnowDesktop.Dock.SourceCloak.Result.v1";
inline constexpr wchar_t AckProperty[] = L"SnowDesktop.Dock.SourceCloak.Ack.v1";

struct Owner
{
    HWND window = nullptr;
    HANDLE process = nullptr;
    HANDLE token = nullptr;
};

inline Owner ReadOwner(HWND source) noexcept
{
    const auto window = reinterpret_cast<HWND>(GetPropW(source, OwnerProperty));
    if (!window) return {};
    return {window,
        GetPropW(source, ProcessProperty), GetPropW(source, TokenProperty)};
}

inline bool SameOwner(HWND source, Owner owner) noexcept
{
    const auto current = ReadOwner(source);
    return current.window == owner.window && current.process == owner.process && current.token == owner.token;
}

inline bool IsLiveOwner(Owner owner) noexcept
{
    DWORD process = 0;
    return owner.window && owner.process && owner.token &&
        GetWindowThreadProcessId(owner.window, &process) && process &&
        owner.process == reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(process)) &&
        GetPropW(owner.window, HostTokenProperty) == owner.token;
}

inline DWORD TaskWindowCloakFlags(HWND source, DWORD flags) noexcept
{
    // Only this animation's app bit is presentation metadata. A Shell or
    // inherited cloak still excludes windows on another virtual desktop.
    const auto owner = ReadOwner(source);
    // A complete stale lease must remain discoverable too: the replacement
    // host first attaches its thread hook, then requests recovery on that thread.
    return (flags & DWM_CLOAKED_APP) && owner.window && owner.process && owner.token
        ? flags & ~static_cast<DWORD>(DWM_CLOAKED_APP) : flags;
}

inline UINT CommandMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(CommandName);
    return message;
}

// DWMWA_CLOAK rejects a caller in a different process. The per-thread
// CALLWNDPROC hook applies it inside the source process and acknowledges the
// current lease; the source application's window procedure is unchanged.
inline void HandleCommand(HWND source, WPARAM cloakValue, LPARAM token) noexcept
{
    const auto owner = ReadOwner(source);
    if (cloakValue > 1 || !owner.window || !owner.process || !owner.token ||
        owner.token != reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(token)) ||
        !SameOwner(source, owner) || (cloakValue && !IsLiveOwner(owner))) return;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(source, &process) || process != GetCurrentProcessId()) return;
    const BOOL cloak = cloakValue != 0;
    const HRESULT hr = DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (!SameOwner(source, owner)) return;
    SetPropW(source, ResultProperty, ULongToHandle(static_cast<ULONG>(hr)));
    SetPropW(source, AckProperty, owner.token);
}

inline HRESULT ApplyCloak(HWND source, Owner owner, BOOL cloak, bool& confirmed) noexcept
{
    confirmed = true;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(source, &process)) return E_INVALIDARG;
    if (process == GetCurrentProcessId())
        return DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak));
    const UINT message = CommandMessage();
    if (!message) return E_NOTIMPL;
    if (!SameOwner(source, owner)) return E_ABORT;
    RemovePropW(source, AckProperty);
    RemovePropW(source, ResultProperty);
    DWORD_PTR ignored = 0;
    SetLastError(0);
    const bool sent = SendMessageTimeoutW(source, message, cloak ? 1 : 0,
        static_cast<LPARAM>(reinterpret_cast<UINT_PTR>(owner.token)),
        SMTO_ABORTIFHUNG, 120, &ignored) != 0;
    const DWORD error = GetLastError();
    if (SameOwner(source, owner) && GetPropW(source, AckProperty) == owner.token)
    {
        const HRESULT hr = static_cast<HRESULT>(HandleToULong(GetPropW(source, ResultProperty)));
        RemovePropW(source, AckProperty);
        RemovePropW(source, ResultProperty);
        return hr;
    }
    // A timed-out sent message may still complete. Retain the lease so cleanup
    // can release a late cloak instead of abandoning an invisible application.
    confirmed = false;
    return sent ? E_NOTIMPL : HRESULT_FROM_WIN32(error ? error : ERROR_TIMEOUT);
}

inline bool RegisterOwner(HWND window) noexcept
{
    static volatile LONG serial = 0;
    ULONG token = static_cast<ULONG>(InterlockedIncrement(&serial));
    if (!token) token = static_cast<ULONG>(InterlockedIncrement(&serial));
    return SetPropW(window, HostTokenProperty,
        reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(token))) != FALSE;
}

inline void RemoveOwner(HWND source, Owner owner) noexcept
{
    if (!SameOwner(source, owner)) return;
    RemovePropW(source, AckProperty);
    RemovePropW(source, ResultProperty);
    RemovePropW(source, TokenProperty);
    RemovePropW(source, ProcessProperty);
    RemovePropW(source, OwnerProperty);
}

// Window properties survive a host crash. Discovery invokes this before its
// cloak eligibility filter, so a surviving app is not permanently excluded.
// PID and host generation also reject a recycled owner HWND.
inline bool RecoverStale(HWND source) noexcept
{
    const auto owner = ReadOwner(source);
    if (!owner.window || !owner.process || !owner.token || IsLiveOwner(owner)) return false;
    const BOOL cloak = FALSE;
    bool confirmed = false;
    if (FAILED(ApplyCloak(source, owner, cloak, confirmed))) return false;
    RemoveOwner(source, owner);
    return true;
}

inline HRESULT Acquire(HWND source, HWND host, bool& acquired) noexcept
{
    acquired = false;
    RecoverStale(source);
    DWORD flags = 0;
    HRESULT hr = DwmGetWindowAttribute(source, DWMWA_CLOAKED, &flags, sizeof(flags));
    if (FAILED(hr)) return hr;
    if (ReadOwner(source).window) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if ((flags & DWM_CLOAKED_APP) != 0) return S_OK;
    Owner owner{host, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId())),
        GetPropW(host, HostTokenProperty)};
    if (!IsLiveOwner(owner)) return E_INVALIDARG;
    if (!SetPropW(source, OwnerProperty, host))
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    if (!SetPropW(source, ProcessProperty, owner.process) || !SetPropW(source, TokenProperty, owner.token))
    {
        const DWORD error = GetLastError();
        // No cloak was set. These partial markers belong to this acquisition.
        if (GetPropW(source, OwnerProperty) == host)
        {
            RemovePropW(source, TokenProperty);
            RemovePropW(source, ProcessProperty);
            RemovePropW(source, OwnerProperty);
        }
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    const BOOL cloak = TRUE;
    bool confirmed = false;
    hr = ApplyCloak(source, owner, cloak, confirmed);
    if (FAILED(hr) && confirmed) RemoveOwner(source, owner);
    else acquired = true;
    return hr;
}

inline HRESULT Release(HWND source, HWND host) noexcept
{
    if (!source || !IsWindow(source)) return S_FALSE;
    const auto owner = ReadOwner(source);
    if (!IsLiveOwner(owner))
        return RecoverStale(source) ? S_OK : S_FALSE;
    if (owner.window != host ||
        owner.process != reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId())))
        return S_FALSE;
    const BOOL cloak = FALSE;
    bool confirmed = false;
    const HRESULT hr = ApplyCloak(source, owner, cloak, confirmed);
    if (SUCCEEDED(hr)) RemoveOwner(source, owner);
    return hr;
}
}
