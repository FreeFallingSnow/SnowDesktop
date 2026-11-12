#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <mutex>
#include <atomic>
#include "dock_cloak_dispatcher.h"
#include "dock_minimize_protocol.h"
#include "dock_window_rules.h"

namespace snowdesktop::dock_source_cloak
{
inline constexpr wchar_t OwnerProperty[] = L"SnowDesktop.Dock.SourceCloak.Owner.v1";
inline constexpr wchar_t ProcessProperty[] = L"SnowDesktop.Dock.SourceCloak.Process.v1";
inline constexpr wchar_t TokenProperty[] = L"SnowDesktop.Dock.SourceCloak.Token.v1";
inline constexpr wchar_t HostTokenProperty[] = L"SnowDesktop.Dock.SourceCloak.LeaseHost.v1";
inline constexpr wchar_t ResultProperty[] = L"SnowDesktop.Dock.SourceCloak.Result.v5";
inline constexpr wchar_t AckProperty[] = L"SnowDesktop.Dock.SourceCloak.Ack.v5";
inline constexpr wchar_t RecoveryProperty[] = L"SnowDesktop.Dock.SourceCloak.Recovery.v1";
inline constexpr wchar_t TimerProperty[] = L"SnowDesktop.Dock.SourceCloak.Timer.v1";

struct Owner
{
    HWND window = nullptr;
    HANDLE process = nullptr;
    HANDLE token = nullptr;
};
inline std::recursive_mutex& LocalLeaseMutex() noexcept
{
    static std::recursive_mutex mutex;
    return mutex;
}
inline Owner RequestOwner(const dock_minimize::CloakRequest& request) noexcept
{
    return {reinterpret_cast<HWND>(static_cast<UINT_PTR>(request.owner)),
        reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(request.process)),
        reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(request.token))};
}
inline std::uint32_t NextRequestId(std::uint32_t operation) noexcept
{
    static std::atomic<std::uint32_t> serial{0};
    auto current = serial.load(std::memory_order_relaxed);
    // Keep IDs representable by an x86 window property. Exhaustion fails closed
    // rather than accepting an ancient acknowledgement after wrapping.
    while (current < 0x3fffffffu)
    {
        if (serial.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
            return ((current + 1) << 2) | operation;
    }
    return 0;
}
inline dock_minimize::CloakRequest MakeRequest(HWND source, Owner owner, std::uint32_t operation,
    std::uint32_t requestId) noexcept
{
    return {5, operation, reinterpret_cast<UINT_PTR>(source),
        reinterpret_cast<UINT_PTR>(owner.window), reinterpret_cast<UINT_PTR>(owner.process),
        reinterpret_cast<UINT_PTR>(owner.token), requestId};
}

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

inline void RemoveOwner(HWND source, Owner owner) noexcept;

inline void StopRecoveryTimer(HWND source) noexcept
{
    const auto timer = reinterpret_cast<UINT_PTR>(RemovePropW(source, TimerProperty));
    if (timer) KillTimer(source, timer);
}

inline void CALLBACK RecoverOnSourceThread(HWND source, UINT, UINT_PTR timer, DWORD) noexcept
{
    const std::lock_guard lock(LocalLeaseMutex());
    if (reinterpret_cast<UINT_PTR>(GetPropW(source, TimerProperty)) != timer) return;
    const auto owner = ReadOwner(source);
    if (!owner.window || !owner.process || !owner.token)
    {
        StopRecoveryTimer(source);
        return;
    }
    if (GetPropW(source, RecoveryProperty) != owner.token && IsLiveOwner(owner)) return;
    const BOOL cloak = FALSE;
    if (SUCCEEDED(DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak))) &&
        SameOwner(source, owner))
    {
        StopRecoveryTimer(source);
        RemoveOwner(source, owner);
    }
}

inline HRESULT StartRecoveryTimer(HWND source) noexcept
{
    if (GetPropW(source, TimerProperty)) return S_OK;
    // This callback must survive removal of both thread hooks and the host.
    // Pin the injected module in this target process before publishing a timer;
    // freeing callback code from inside its own TIMERPROC is not safe.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&RecoverOnSourceThread), &module))
        return HRESULT_FROM_WIN32(GetLastError());
    // With a non-null HWND, KillTimer and TIMERPROC use nIDEvent; SetTimer's
    // return value is only a success indicator, not the cancellation contract.
    const UINT_PTR timer = reinterpret_cast<UINT_PTR>(&RecoverOnSourceThread);
    if (!SetTimer(source, timer, 100, RecoverOnSourceThread))
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    if (!SetPropW(source, TimerProperty, reinterpret_cast<HANDLE>(timer)))
    {
        const DWORD error = GetLastError();
        KillTimer(source, timer);
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    return S_OK;
}

struct RestoreCloakContext
{
    HWND source;
    Owner owner;
    int command;
    bool called = false;
    HRESULT result = E_ABORT;
};
inline thread_local RestoreCloakContext* restoreCloakContext = nullptr;

inline LRESULT CALLBACK CloakBeforeRestore(int code, WPARAM wp, LPARAM lp) noexcept
{
    auto* context = restoreCloakContext;
    if (code != HCBT_MINMAX || !context || context->called ||
        reinterpret_cast<HWND>(wp) != context->source || LOWORD(lp) != context->command)
        return CallNextHookEx(nullptr, code, wp, lp);
    context->called = true;
    const LRESULT next = CallNextHookEx(nullptr, code, wp, lp);
    if (next) return next;
    if (!SameOwner(context->source, context->owner) || !IsLiveOwner(context->owner) ||
        GetPropW(context->source, RecoveryProperty) == context->owner.token || !IsIconic(context->source))
        return 1;
    const BOOL cloak = TRUE;
    context->result = DwmSetWindowAttribute(context->source, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (SUCCEEDED(context->result) && GetPropW(context->source, RecoveryProperty) == context->owner.token)
        context->result = E_ABORT;
    // A failed hide must not let this preparation expose an expanded window.
    return FAILED(context->result) ? 1 : 0;
}

inline HRESULT RestoreWithCloak(HWND source, Owner owner) noexcept
{
    if (restoreCloakContext || GetPropW(source, RecoveryProperty) == owner.token)
        return E_ABORT;
    if (!IsIconic(source))
    {
        // The same sent message can pass another installed cloak hook. A
        // prepared source needs no second native restore (especially when max).
        DWORD flags = 0;
        return SUCCEEDED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &flags, sizeof(flags))) &&
            (flags & DWM_CLOAKED_APP) ? S_OK : E_ABORT;
    }
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(source, &placement)) return E_FAIL;
    RestoreCloakContext context{source, owner,
        dock_window_rules::ResolveDockRestoreShowCommand(placement.flags, placement.showCmd)};
    const HHOOK hook = SetWindowsHookExW(WH_CBT, CloakBeforeRestore, nullptr, GetCurrentThreadId());
    if (!hook) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
    // A reversal can retain a lease from the preceding minimize. Uncloak only
    // while iconic, so Windows can process restore-related pending recalc state.
    // Its HCBT_MINMAX callback then hides the source before geometry is expanded,
    // rather than after ShowWindow returns and exposes native animation frames.
    const BOOL cloak = FALSE;
    HRESULT hr = DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (SUCCEEDED(hr))
    {
        restoreCloakContext = &context;
        ShowWindow(source, context.command);
        restoreCloakContext = nullptr;
        hr = context.result;
        if (SUCCEEDED(hr) && (!SameOwner(source, owner) ||
            GetPropW(source, RecoveryProperty) == owner.token || IsIconic(source))) hr = E_ABORT;
    }
    UnhookWindowsHookEx(hook);
    return hr;
}

// DWMWA_CLOAK rejects a caller in a different process. The per-thread
// CALLWNDPROC hook applies it inside the source process and acknowledges the
// exact request before the source window procedure sees the registered message.
inline void HandleCommand(HWND source, const dock_minimize::CloakRequest& request) noexcept
{
    const std::lock_guard lock(LocalLeaseMutex());
    const auto owner = RequestOwner(request);
    if (request.source != reinterpret_cast<UINT_PTR>(source) || !SameOwner(source, owner) ||
        (request.cloak && !IsLiveOwner(owner))) return;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(source, &process) || process != GetCurrentProcessId()) return;
    const BOOL cloak = request.cloak != 0;
    const bool restore = request.cloak == dock_minimize::kPrepareRestoreCloak;
    const bool cancelled = cloak && GetPropW(source, RecoveryProperty) == owner.token;
    const bool expired = cloak && !dock_minimize::CloakSendBudget(owner.window, 1);
    HRESULT hr = cancelled ? E_ABORT : expired ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) :
        cloak ? StartRecoveryTimer(source) : S_OK;
    if (SUCCEEDED(hr)) hr = restore ? RestoreWithCloak(source, owner) :
        DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (cloak && SUCCEEDED(hr) && !dock_minimize::CloakSendBudget(owner.window, 1))
    {
        // The native CBT request can be released as soon as this send returns.
        // Undo a cloak that completed after its animation deadline.
        const BOOL uncloak = FALSE;
        if (FAILED(DwmSetWindowAttribute(source, DWMWA_CLOAK, &uncloak, sizeof(uncloak))))
            SetPropW(source, RecoveryProperty, owner.token);
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    // Restore can fail after hiding (a veto, cancellation, or application
    // reentrancy). Keep its lease and timer until explicit release/recovery.
    if ((!restore && cloak && FAILED(hr) && GetPropW(source, RecoveryProperty) != owner.token) ||
        (!cloak && SUCCEEDED(hr))) StopRecoveryTimer(source);
    if (!SameOwner(source, owner)) return;
    // Invalidate before changing the payload, then publish the request ID last.
    // A reader checks the ID on both sides of its payload read.
    RemovePropW(source, AckProperty);
    if (SetPropW(source, ResultProperty, ULongToHandle(static_cast<ULONG>(hr))))
        SetPropW(source, AckProperty, ULongToHandle(static_cast<ULONG>(request.requestId)));
}

inline HRESULT ApplyCloak(HWND source, Owner owner, std::uint32_t operation, bool& confirmed) noexcept
{
    confirmed = true;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(source, &process)) return E_INVALIDARG;
    std::uint32_t requestId = 0;
    {
        const std::lock_guard lock(LocalLeaseMutex());
        if (!SameOwner(source, owner) || (operation && GetPropW(source, RecoveryProperty) == owner.token))
            return E_ABORT;
        if (process == GetCurrentProcessId() && operation != dock_minimize::kPrepareRestoreCloak)
        {
            const BOOL cloak = operation != 0;
            return DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloak, sizeof(cloak));
        }
        requestId = NextRequestId(operation);
        if (!requestId) return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        RemovePropW(source, AckProperty);
        RemovePropW(source, ResultProperty);
    }
    const HWND dispatcher = reinterpret_cast<HWND>(GetPropW(source, dock_minimize::kCloakDispatcherProperty));
    if (process != GetCurrentProcessId() && !dispatcher)
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    const DWORD maximum = dispatcher ? 180 : 120;
    const DWORD timeout = operation ? dock_minimize::CloakSendBudget(owner.window, maximum) : maximum;
    if (!timeout) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    const auto acknowledgement = ULongToHandle(requestId);
    DWORD_PTR ignored = 0;
    SetLastError(0);
    const auto request = MakeRequest(source, owner, operation, requestId);
    bool sent = true;
    if (process == GetCurrentProcessId()) HandleCommand(source, request);
    else sent = dock_minimize::SendCloakDispatchRequest(dispatcher, request, timeout, ignored);
    const DWORD error = GetLastError();
    const std::lock_guard lock(LocalLeaseMutex());
    if (SameOwner(source, owner) && GetPropW(source, AckProperty) == acknowledgement)
    {
        const HRESULT hr = static_cast<HRESULT>(HandleToULong(GetPropW(source, ResultProperty)));
        if (GetPropW(source, AckProperty) == acknowledgement && SameOwner(source, owner))
        {
            RemovePropW(source, AckProperty);
            RemovePropW(source, ResultProperty);
            return hr;
        }
    }
    if (dispatcher && sent && FAILED(static_cast<HRESULT>(ignored)) &&
        static_cast<HRESULT>(ignored) != HRESULT_FROM_WIN32(ERROR_TIMEOUT))
        return static_cast<HRESULT>(ignored);
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
    RemovePropW(source, RecoveryProperty);
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
    const std::lock_guard lock(LocalLeaseMutex());
    const auto owner = ReadOwner(source);
    if (!owner.window || !owner.process || !owner.token || IsLiveOwner(owner)) return false;
    const BOOL cloak = FALSE;
    bool confirmed = false;
    if (FAILED(ApplyCloak(source, owner, cloak, confirmed))) return false;
    RemoveOwner(source, owner);
    return true;
}

inline Owner CaptureOwner(HWND host) noexcept
{
    return {host, ULongToHandle(GetCurrentProcessId()), GetPropW(host, HostTokenProperty)};
}

// Reserve on the transition thread. The send can then use this exact lease on
// a worker without holding the process mutex across a foreign window call.
inline HRESULT Reserve(HWND source, Owner owner, bool& reserved, bool restore) noexcept
{
    const std::lock_guard lock(LocalLeaseMutex());
    reserved = false;
    RecoverStale(source);
    DWORD flags = 0;
    HRESULT hr = DwmGetWindowAttribute(source, DWMWA_CLOAKED, &flags, sizeof(flags));
    if (FAILED(hr)) return hr;
    if (!IsLiveOwner(owner)) return E_INVALIDARG;
    if (ReadOwner(source).window)
    {
        if (!restore || !SameOwner(source, owner)) return HRESULT_FROM_WIN32(ERROR_BUSY);
        reserved = true;
        return S_OK;
    }
    if ((flags & DWM_CLOAKED_APP) != 0) return restore ? E_ABORT : S_OK;
    if (!SetPropW(source, OwnerProperty, owner.window))
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    if (!SetPropW(source, ProcessProperty, owner.process) || !SetPropW(source, TokenProperty, owner.token))
    {
        const DWORD error = GetLastError();
        // No cloak was set. These partial markers belong to this acquisition.
        if (GetPropW(source, OwnerProperty) == owner.window)
        {
            RemovePropW(source, TokenProperty);
            RemovePropW(source, ProcessProperty);
            RemovePropW(source, OwnerProperty);
        }
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    reserved = true;
    return S_OK;
}

inline HRESULT Acquire(HWND source, HWND host, bool& acquired, bool restore = false) noexcept
{
    const std::lock_guard lock(LocalLeaseMutex());
    const auto owner = CaptureOwner(host);
    HRESULT hr = Reserve(source, owner, acquired, restore);
    if (FAILED(hr) || !acquired) return hr;
    bool confirmed = false;
    hr = ApplyCloak(source, owner, restore ? dock_minimize::kPrepareRestoreCloak : 1u, confirmed);
    if (FAILED(hr) && confirmed && !restore && GetPropW(source, RecoveryProperty) != owner.token)
    {
        RemoveOwner(source, owner);
        acquired = false;
    }
    return hr;
}

inline HRESULT Release(HWND source, Owner owner) noexcept
{
    {
        const std::lock_guard lock(LocalLeaseMutex());
        if (!source || !IsWindow(source) || !SameOwner(source, owner)) return S_FALSE;
        // The source's timer can finish the release even if the hook is gone.
        if (!SetPropW(source, RecoveryProperty, owner.token))
        {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
        }
    }
    bool confirmed = false;
    const HRESULT hr = ApplyCloak(source, owner, 0, confirmed);
    if (SUCCEEDED(hr))
    {
        const std::lock_guard lock(LocalLeaseMutex());
        RemoveOwner(source, owner);
    }
    return hr;
}
inline HRESULT Release(HWND source, HWND host) noexcept
{
    Owner owner;
    {
        const std::lock_guard lock(LocalLeaseMutex());
        owner = ReadOwner(source);
        if (owner.window != host || owner.process != ULongToHandle(GetCurrentProcessId())) return S_FALSE;
    }
    return Release(source, owner);
}
}
