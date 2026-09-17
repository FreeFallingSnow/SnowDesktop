#include "taskbar_autohide_observer.h"
#include "taskbar_autohide_rules.h"
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
constexpr wchar_t kProtectedTaskbar[] = L"SnowDesktop.Taskbar.AutoHideActivation.v8";
std::atomic<std::uintptr_t> moduleBase{0};
std::atomic<AutoHideTraceBuffer*> output{nullptr};
std::once_flag adapterOnce;
LONG adapterStatus = 0;
thread_local ULONGLONG rateWindow = 0;
thread_local unsigned rateCount = 0;
struct Activation
{
    HWND taskbar = nullptr, previous = nullptr;
    LONG value = 0;
};
thread_local Activation activation;

// Microsoft public PDB: Taskbar.pdb BC068C70D2B5878D1FBBAFD6824EC5D2 age 1.
// Offline inspection of this exact image found WM_ACTIVATE -> Unhide(0, 8)
// in both TrayUI::WndProc and CSecondaryTray::v_WndProc. Explicit focus also
// reaches that branch, so both TaskbarHost focus paths must be hooked together.
// No assumption is made that other Windows builds share this ABI/RVA.
constexpr GUID kPdb = {0xbc068c70, 0xd2b5, 0x878d,
    {0x1f, 0xbb, 0xaf, 0xd6, 0x82, 0x4e, 0xc5, 0xd2}};
constexpr DWORD kImageSize = 0x308000, kTimestamp = 0x01dffebf;

bool IsSupportedImage(HMODULE module) noexcept
{
    if (!module) return false;
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 4096) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG ||
        nt->FileHeader.TimeDateStamp != kTimestamp ||
        nt->OptionalHeader.SizeOfImage != kImageSize) return false;
    const auto& debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!debug.VirtualAddress || debug.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
        debug.VirtualAddress > kImageSize || debug.Size > kImageSize - debug.VirtualAddress)
        return false;
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + debug.VirtualAddress);
    for (DWORD i = 0; i < debug.Size / sizeof(*entries); ++i)
    {
        const auto& entry = entries[i];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < 24 ||
            entry.AddressOfRawData > kImageSize - 24) continue;
        const BYTE* codeview = base + entry.AddressOfRawData;
        DWORD age = 0;
        std::memcpy(&age, codeview + 20, sizeof(age));
        if (std::memcmp(codeview, "RSDS", 4) == 0 && age == 1 &&
            std::memcmp(codeview + 4, &kPdb, sizeof(kPdb)) == 0) return true;
    }
    return false;
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
    if (caller && offset < kImageSize) record.callerRva = static_cast<DWORD>(offset);
    AppendAutoHideTrace(*buffer, record);
}

ActivationRevealContext ReadRevealContext(HWND taskbar, bool queryGeometry)
{
    ActivationRevealContext context;
    context.protectedTaskbar = output.load(std::memory_order_acquire) && adapterReady.load() &&
        taskbar && GetPropW(taskbar, kProtectedTaskbar);
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
    // This exact image routes native taskbar/notification-area focus through
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
    if (offset < kImageSize) context.callerRva = static_cast<DWORD>(offset);
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

LONG InstallAdapter() noexcept
{
    const HMODULE taskbarModule = GetModuleHandleW(L"Taskbar.dll");
    if (!IsSupportedImage(taskbarModule)) return -1;
    moduleBase.store(reinterpret_cast<std::uintptr_t>(taskbarModule), std::memory_order_release);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return -2;
    auto* base = reinterpret_cast<BYTE*>(taskbarModule);
    struct Entry { DWORD rva; void* callback; void** original; BYTE prologue[5]; };
    const Entry entries[] = {
        {0x924c0, reinterpret_cast<void*>(&Primary), reinterpret_cast<void**>(&primaryOriginal),
            {0x48, 0x89, 0x5c, 0x24, 0x18}},
        {0x14cca8, reinterpret_cast<void*>(&Secondary), reinterpret_cast<void**>(&secondaryOriginal),
            {0x48, 0x89, 0x5c, 0x24, 0x18}},
        // Microsoft PDB confirms void TaskbarHost::SetFocusOnTaskbar(), and
        // bool TaskbarHost::SetFocusWithCommand(TaskbarCommand, bool).
        {0x84de0, reinterpret_cast<void*>(&SetFocus), reinterpret_cast<void**>(&focusOriginal),
            {0x48, 0x83, 0xec, 0x28, 0x48}},
        {0x123e30, reinterpret_cast<void*>(&SetFocusWithCommand), reinterpret_cast<void**>(&focusCommandOriginal),
            {0x48, 0x89, 0x5c, 0x24, 0x08}},
        // void TrayUI::HandleTaskbarHotkey(WPARAM) and
        // void TrayUI::_OnFocusMsg(UINT, WPARAM, LPARAM), verified in the PDB.
        {0xdc860, reinterpret_cast<void*>(&HandleTaskbarHotkey), reinterpret_cast<void**>(&hotkeyOriginal),
            {0x48, 0x89, 0x5c, 0x24, 0x10}},
        {0xe7ecc, reinterpret_cast<void*>(&OnFocusMessage), reinterpret_cast<void**>(&focusMessageOriginal),
            {0x48, 0x89, 0x5c, 0x24, 0x08}},
    };
    for (const auto& entry : entries)
        if (std::memcmp(base + entry.rva, entry.prologue, sizeof(entry.prologue)) != 0) return -3;
    size_t created = 0;
    for (const auto& entry : entries)
    {
        if (MH_CreateHook(base + entry.rva, entry.callback, entry.original) != MH_OK)
        {
            // Nothing has been enabled yet, so removing these is safe.
            for (size_t i = 0; i < created; ++i) MH_RemoveHook(base + entries[i].rva);
            return -4;
        }
        ++created;
    }
    bool queued = true;
    for (const auto& entry : entries)
        queued = (MH_QueueEnableHook(base + entry.rva) == MH_OK) && queued;
    if (!queued || MH_ApplyQueued() != MH_OK)
    {
        // Keep trampolines alive for calls already in flight. Filtering stays
        // disabled unless every focus and reveal hook was enabled together.
        for (const auto& entry : entries) MH_DisableHook(base + entry.rva);
        return -6;
    }
    adapterReady.store(true, std::memory_order_release);
    return 1;
}
}

void Disable() noexcept
{
    output.store(nullptr, std::memory_order_release);
}

void Configure(AutoHideTraceBuffer* buffer, HWND taskbar, bool observe, bool protectActivation) noexcept
{
    if (!observe)
    {
        if (taskbar && RemovePropW(taskbar, kProtectedTaskbar))
            Record(AutoHideTraceKind::Protection, 0, 0, nullptr, taskbar);
        Disable();
        return;
    }
    try
    {
        // The TAP keeps an explicit module reference for Explorer's lifetime.
        // Its shared mapping also stays mapped until process detach. Retained
        // trampolines can therefore always call the original after shutdown.
        std::call_once(adapterOnce, [] { adapterStatus = InstallAdapter(); });
        if (!output.exchange(buffer, std::memory_order_acq_rel))
            Record(AutoHideTraceKind::Adapter, adapterStatus);
        if (taskbar)
        {
            const bool protectedBefore = GetPropW(taskbar, kProtectedTaskbar) != nullptr;
            const bool requestedProtection = protectActivation && adapterReady.load();
            if (requestedProtection)
            {
                if (!GetPropW(taskbar, kProtectedTaskbar))
                    SetPropW(taskbar, kProtectedTaskbar, reinterpret_cast<HANDLE>(1));
            }
            else RemovePropW(taskbar, kProtectedTaskbar);
            if (protectedBefore != requestedProtection)
                Record(AutoHideTraceKind::Protection,
                    GetPropW(taskbar, kProtectedTaskbar) ? 1 : 0,
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
