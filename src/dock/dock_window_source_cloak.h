#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace snowdesktop::dock_source_cloak
{
inline constexpr wchar_t OwnerProperty[] = L"SnowDesktop.Dock.SourceCloak.Owner.v1";
inline constexpr wchar_t ProcessProperty[] = L"SnowDesktop.Dock.SourceCloak.Process.v1";
inline constexpr wchar_t TokenProperty[] = L"SnowDesktop.Dock.SourceCloak.Token.v1";
inline constexpr wchar_t HostTokenProperty[] = L"SnowDesktop.Dock.SourceCloak.LeaseHost.v1";

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
    return (flags & DWM_CLOAKED_APP) && IsLiveOwner(ReadOwner(source))
        ? flags & ~static_cast<DWORD>(DWM_CLOAKED_APP) : flags;
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
    if (!owner.window || IsLiveOwner(owner)) return false;
    const BOOL cloak = FALSE;
    if (FAILED(DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak)))) return false;
    RemoveOwner(source, owner);
    return true;
}

inline HRESULT Acquire(HWND source, HWND host, bool& acquired) noexcept
{
    acquired = false;
    RecoverStale(source);
    DWORD flags = 0;
    HRESULT hr = DwmGetWindowAttribute(source, DWMWA_CLOAKED, &flags, sizeof(flags));
    if (FAILED(hr) || (flags & DWM_CLOAKED_APP) != 0) return hr;
    if (ReadOwner(source).window) return HRESULT_FROM_WIN32(ERROR_BUSY);
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
    hr = DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (FAILED(hr)) RemoveOwner(source, owner);
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
    const HRESULT hr = DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (SUCCEEDED(hr)) RemoveOwner(source, owner);
    return hr;
}
}
