#include "taskbar_autohide_observer.h"
#include "taskbar_autohide_rules.h"
#include "taskbar_hook_lifecycle.h"
#include <MinHook.h>
#include <commctrl.h>
#include <intrin.h>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <mutex>

namespace snowdesktop::taskbar_hook::autohide_observer
{
namespace
{
using Unhide = void(WINAPI*)(void*, int, int);
Unhide primaryOriginal = nullptr, secondaryOriginal = nullptr;
using Focus = void(WINAPI*)(void*);
using FocusCommand = bool(WINAPI*)(void*, int, bool);
Focus focusOriginal = nullptr;
FocusCommand focusCommandOriginal = nullptr;
using Hotkey = void(WINAPI*)(void*, WPARAM);
using FocusMessage = void(WINAPI*)(void*, UINT, WPARAM, LPARAM);
Hotkey hotkeyOriginal = nullptr;
FocusMessage focusMessageOriginal = nullptr;
std::atomic<unsigned> explicitFocusCalls{0};
std::atomic<bool> adapterReady{false};
std::atomic<std::uintptr_t> moduleBase{0};
std::atomic<DWORD> moduleSize{0};
std::atomic<AutoHideTraceBuffer*> output{nullptr};
std::mutex adapterMutex;
AutoHideAdapter installedAdapter;
bool adapterAttempted = false;
ULONGLONG lastAdapterAttempt = 0;
RetainedHookSet<6> revealHooks;
LONG adapterStatus = 0;
DWORD reportedResolutionError = ERROR_IO_PENDING;
thread_local ULONGLONG rateWindow = 0;
thread_local unsigned rateCount = 0;
struct Activation
{
    HWND taskbar = nullptr, previous = nullptr;
    LONG value = 0;
};
thread_local Activation activation;

std::optional<AutoHideImageView> LoadedImage(HMODULE module) noexcept
{
    if (!module) return {};
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const AutoHideImageView header({base, 4096});
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!header.Read(0, dos) || dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
        !header.Read(static_cast<std::size_t>(dos.e_lfanew), nt) ||
        !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > 64 * 1024 * 1024) return {};
    return AutoHideImageView({base, nt.OptionalHeader.SizeOfImage});
}

void Record(AutoHideTraceKind kind, int flags = 0, int request = 0,
    void* caller = nullptr, HWND observedWindow = nullptr) noexcept
{
    auto* buffer = output.load(std::memory_order_acquire);
    if (!buffer) return;
    struct RestoreLastError
    {
        DWORD value = GetLastError();
        ~RestoreLastError() { SetLastError(value); }
    } restoreLastError;
    const ULONGLONG now = GetTickCount64();
    if (now - rateWindow >= 1000) { rateWindow = now; rateCount = 0; }
    if (++rateCount > 64)
    {
        InterlockedIncrement(&buffer->dropped);
        return;
    }
    AutoHideTraceRecord record;
    record.tick = now;
    record.kind = kind;
    record.threadId = GetCurrentThreadId();
    const HWND taskbar = observedWindow ? observedWindow : activation.taskbar;
    record.taskbar = reinterpret_cast<std::uintptr_t>(taskbar);
    record.previous = reinterpret_cast<std::uintptr_t>(activation.previous);
    record.foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
    record.activation = activation.value;
    record.previousIconic = activation.previous && IsIconic(activation.previous);
    const bool cursorValid = GetCursorPos(&record.cursor) != FALSE;
    record.geometryValid = GetWindowRect(taskbar, &record.rect) && cursorValid;
    record.flags = flags;
    record.request = request;
    record.explicitFocus = static_cast<LONG>(explicitFocusCalls.load(std::memory_order_acquire));
    const auto offset = reinterpret_cast<std::uintptr_t>(caller) -
        moduleBase.load(std::memory_order_acquire);
    if (caller && offset < moduleSize.load(std::memory_order_acquire)) record.callerRva = static_cast<DWORD>(offset);
    AppendAutoHideTrace(*buffer, record);
}

ActivationRevealContext ReadRevealContext(HWND taskbar, bool queryGeometry)
{
    ActivationRevealContext context;
    context.protectedTaskbar = output.load(std::memory_order_acquire) && adapterReady.load() &&
        taskbar && GetPropW(taskbar, kActivationProtectionProperty);
    context.explicitFocus = explicitFocusCalls.load(std::memory_order_acquire) != 0;
    if (context.protectedTaskbar && queryGeometry)
    {
        MONITORINFO monitor{sizeof(monitor)};
        context.geometryValid = GetWindowRect(taskbar, &context.taskbar) &&
            GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONULL), &monitor) &&
            GetCursorPos(&context.cursor);
        context.monitor = monitor.rcMonitor;
    }
    return context;
}

struct ExplicitFocusScope
{
    bool active;
    int source, command;
    ExplicitFocusScope(bool isActive, int origin, int value = 0) noexcept
        : active(isActive), source(origin), command(value)
    {
        if (!active) return;
        explicitFocusCalls.fetch_add(1, std::memory_order_acq_rel);
        Record(AutoHideTraceKind::FocusEnter, source, command);
    }
    ~ExplicitFocusScope()
    {
        if (!active) return;
        Record(AutoHideTraceKind::FocusLeave, source, command);
        explicitFocusCalls.fetch_sub(1, std::memory_order_acq_rel);
    }
    ExplicitFocusScope(const ExplicitFocusScope&) = delete;
    ExplicitFocusScope& operator=(const ExplicitFocusScope&) = delete;
};

void WINAPI SetFocus(void* self)
{
    ExplicitFocusScope scope(true, 1);
    focusOriginal(self);
}
bool WINAPI SetFocusWithCommand(void* self, int command, bool fromDesktop)
{
    ExplicitFocusScope scope(true, 2, command);
    return focusCommandOriginal(self, command, fromDesktop);
}

bool IsTaskbarHotkey(WPARAM id) noexcept
{
    return id == 0x1fe || id == 0x1ff || id == 0x24e;
}

void RevealForExplicitFocus(void* tray)
{
    const DWORD savedError = GetLastError();
    const HWND foreground = GetForegroundWindow();
    wchar_t className[64]{};
    const bool primaryForeground = foreground &&
        GetClassNameW(foreground, className, 64) &&
        std::wcscmp(className, L"Shell_TrayWnd") == 0;
    const auto context = ReadRevealContext(primaryForeground ? foreground : nullptr, true);
    SetLastError(savedError);
    if (DispatchExplicitForegroundReveal(context, primaryForeground, [&](int flags, int request) {
        primaryOriginal(tray, flags, request);
    }))
        Record(AutoHideTraceKind::ExplicitReveal, 0, 8, nullptr, foreground);
}

void WINAPI HandleTaskbarHotkey(void* self, WPARAM id)
{
    const bool explicitRequest = IsTaskbarHotkey(id);
    ExplicitFocusScope scope(explicitRequest, 4, static_cast<int>(id));
    if (explicitRequest) RevealForExplicitFocus(self);
    hotkeyOriginal(self, id);
}

void WINAPI OnFocusMessage(void* self, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Native taskbar/notification-area focus is routed through
    // _OnFocusMsg. Zero wParam gives focus back to the desktop and is excluded.
    const bool explicitRequest = (message == 0x55c || message == 0x55d) && wParam != 0;
    ExplicitFocusScope scope(explicitRequest, 5, static_cast<int>(message));
    if (explicitRequest) RevealForExplicitFocus(self);
    focusMessageOriginal(self, message, wParam, lParam);
}

void Reveal(void* self, int flags, int request, bool secondary, void* caller)
{
    const DWORD savedError = GetLastError();
    Record(secondary ? AutoHideTraceKind::SecondaryUnhide : AutoHideTraceKind::PrimaryUnhide,
        flags, request, caller);
    auto context = ReadRevealContext(activation.taskbar, flags == 0 && request == 8);
    context.secondary = secondary;
    context.activation = activation.value;
    const auto offset = reinterpret_cast<std::uintptr_t>(caller) - moduleBase.load(std::memory_order_acquire);
    if (adapterReady.load(std::memory_order_acquire) && offset < installedAdapter.image.imageSize)
        context.callerIsActivationHandler = installedAdapter.IsActivationCaller(secondary, static_cast<DWORD>(offset));
    SetLastError(savedError);
    if (DispatchActivationReveal(context, flags, request, [&](int originalFlags, int originalRequest) {
        (secondary ? secondaryOriginal : primaryOriginal)(self, originalFlags, originalRequest);
    }))
        Record(AutoHideTraceKind::SuppressedActivation, flags, request, caller);
}
void WINAPI Primary(void* self, int flags, int request)
{
    Reveal(self, flags, request, false, _ReturnAddress());
}
void WINAPI Secondary(void* self, int flags, int request)
{
    Reveal(self, flags, request, true, _ReturnAddress());
}

bool ChangeHook(void* target, bool enable) noexcept
{
    const MH_STATUS result = enable ? MH_EnableHook(target) : MH_DisableHook(target);
    return result == MH_OK || result == (enable ? MH_ERROR_ENABLED : MH_ERROR_DISABLED);
}

LONG InstallAdapter(const AutoHideAdapter& adapter) noexcept
{
    const HMODULE taskbarModule = GetModuleHandleW(L"Taskbar.dll");
    const auto image = LoadedImage(taskbarModule);
    if (!image || !image->Validate(adapter)) return -1;
    installedAdapter = adapter;
    moduleSize.store(adapter.image.imageSize, std::memory_order_release);
    moduleBase.store(reinterpret_cast<std::uintptr_t>(taskbarModule), std::memory_order_release);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return -2;
    auto* base = reinterpret_cast<BYTE*>(taskbarModule);
    struct Entry { AutoHideSymbol symbol; void* callback; void** original; };
    const Entry entries[] = {
        {AutoHideSymbol::PrimaryUnhide, reinterpret_cast<void*>(&Primary), reinterpret_cast<void**>(&primaryOriginal)},
        {AutoHideSymbol::SecondaryUnhide, reinterpret_cast<void*>(&Secondary), reinterpret_cast<void**>(&secondaryOriginal)},
        {AutoHideSymbol::Focus, reinterpret_cast<void*>(&SetFocus), reinterpret_cast<void**>(&focusOriginal)},
        {AutoHideSymbol::FocusCommand, reinterpret_cast<void*>(&SetFocusWithCommand), reinterpret_cast<void**>(&focusCommandOriginal)},
        {AutoHideSymbol::Hotkey, reinterpret_cast<void*>(&HandleTaskbarHotkey), reinterpret_cast<void**>(&hotkeyOriginal)},
        {AutoHideSymbol::FocusMessage, reinterpret_cast<void*>(&OnFocusMessage), reinterpret_cast<void**>(&focusMessageOriginal)},
    };
    for (const auto& entry : entries)
    {
        void* target = base + adapter.Get(entry.symbol).begin;
        if (revealHooks.Contains(target)) continue;
        if (MH_CreateHook(target, entry.callback, entry.original) != MH_OK ||
            !revealHooks.Remember(target)) return -4;
    }
    if (!revealHooks.Enable(ChangeHook)) return -6;
    adapterReady.store(true, std::memory_order_release);
    return 1;
}

bool DisableLocked() noexcept
{
    output.store(nullptr, std::memory_order_release);
    adapterReady.store(false, std::memory_order_release);
    adapterAttempted = false;
    lastAdapterAttempt = 0;
    return revealHooks.Disable(ChangeHook);
}
}

bool Disable() noexcept
{
    try
    {
        std::lock_guard lock(adapterMutex);
        return DisableLocked();
    }
    catch (...) { output.store(nullptr, std::memory_order_release); return false; }
}

void Configure(AutoHideTraceBuffer* buffer, HWND taskbar, bool observe, bool protectActivation,
    const AutoHideAdapter& adapter, DWORD resolutionError) noexcept
{
    try
    {
        // The TAP keeps an explicit module reference for Explorer's lifetime.
        // Its shared mapping also stays mapped until process detach. Retained
        // trampolines can therefore always call the original after shutdown.
        std::lock_guard lock(adapterMutex);
        // A queued Apply from before retirement must neither re-enable the
        // old detours nor clear protection properties owned by its successor.
        if (hookInstanceRetired.load()) { DisableLocked(); return; }
        if (!observe)
        {
            if (taskbar && RemovePropW(taskbar, kActivationProtectionProperty))
                Record(AutoHideTraceKind::Protection, 0, 0, nullptr, taskbar);
            DisableLocked();
            return;
        }
        const bool firstObservation = !output.exchange(buffer, std::memory_order_acq_rel);
        bool attemptedNow = false;
        // Pending symbols or a not-yet-loaded Taskbar.dll must not consume the
        // installation attempt. The host publishes and posts Apply after resolve.
        const ULONGLONG now = GetTickCount64();
        const LONG previousStatus = adapterStatus;
        if (!adapterReady.load(std::memory_order_acquire) &&
            (!adapterAttempted || now - lastAdapterAttempt >= 1000) &&
            resolutionError == ERROR_SUCCESS && GetModuleHandleW(L"Taskbar.dll"))
        {
            adapterAttempted = true;
            lastAdapterAttempt = now;
            attemptedNow = true;
            adapterStatus = InstallAdapter(adapter);
        }
        if (firstObservation || (attemptedNow && previousStatus != adapterStatus) ||
            reportedResolutionError != resolutionError)
            Record(AutoHideTraceKind::Adapter, adapterStatus, static_cast<int>(resolutionError));
        reportedResolutionError = resolutionError;
        if (taskbar)
        {
            const bool protectedBefore = GetPropW(taskbar, kActivationProtectionProperty) != nullptr;
            const bool requestedProtection = protectActivation && adapterReady.load();
            if (requestedProtection)
            {
                if (!GetPropW(taskbar, kActivationProtectionProperty))
                    SetPropW(taskbar, kActivationProtectionProperty, reinterpret_cast<HANDLE>(1));
            }
            else RemovePropW(taskbar, kActivationProtectionProperty);
            if (protectedBefore != requestedProtection)
                Record(AutoHideTraceKind::Protection,
                    GetPropW(taskbar, kActivationProtectionProperty) ? 1 : 0,
                    requestedProtection ? 1 : 0, nullptr, taskbar);
        }
    }
    catch (...) { output.store(nullptr, std::memory_order_release); }
}

LRESULT Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Native keyboard taskbar commands remain explicit even if focus crosses
    // threads during a synchronous XAML call; the counter is not a timed lease.
    // These three IDs are dispatched by this image's HandleTaskbarHotkey.
    // Do not grant the same bypass to unrelated shortcuts such as Show Desktop.
    const bool taskbarHotkey = message == WM_HOTKEY && IsTaskbarHotkey(wParam);
    ExplicitFocusScope focus(taskbarHotkey ||
        (message == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_TASKLIST), 3,
        static_cast<int>(message));
    if (message != WM_ACTIVATE || !output.load(std::memory_order_acquire))
        return DefSubclassProc(window, message, wParam, lParam);
    const Activation previous = activation;
    activation = {window, reinterpret_cast<HWND>(lParam), LOWORD(wParam)};
    struct Restore
    {
        Activation previous;
        ~Restore() { activation = previous; }
    } restore{previous};
    Record(AutoHideTraceKind::ActivateBefore);
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    Record(AutoHideTraceKind::ActivateAfter);
    return result;
}
}
