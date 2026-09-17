#include "taskbar_autohide_observer.h"
#include <MinHook.h>
#include <commctrl.h>
#include <intrin.h>
#include <cstring>
#include <atomic>
#include <mutex>

namespace snowdesktop::taskbar_hook::autohide_observer
{
namespace
{
using Unhide = void(WINAPI*)(void*, int, int);
Unhide primaryOriginal = nullptr, secondaryOriginal = nullptr;
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
// in both TrayUI::WndProc and CSecondaryTray::v_WndProc. These are diagnostic
// adapters, not an assumption that every Windows build shares this ABI/RVA.
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
    void* caller = nullptr) noexcept
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
    record.taskbar = reinterpret_cast<std::uintptr_t>(activation.taskbar);
    record.previous = reinterpret_cast<std::uintptr_t>(activation.previous);
    record.foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
    record.activation = activation.value;
    record.previousIconic = activation.previous && IsIconic(activation.previous);
    const bool cursorValid = GetCursorPos(&record.cursor) != FALSE;
    record.geometryValid = GetWindowRect(activation.taskbar, &record.rect) && cursorValid;
    record.flags = flags;
    record.request = request;
    const auto offset = reinterpret_cast<std::uintptr_t>(caller) -
        moduleBase.load(std::memory_order_acquire);
    if (caller && offset < kImageSize) record.callerRva = static_cast<DWORD>(offset);
    AppendAutoHideTrace(*buffer, record);
}

void WINAPI Primary(void* self, int flags, int request)
{
    Record(AutoHideTraceKind::PrimaryUnhide, flags, request, _ReturnAddress());
    primaryOriginal(self, flags, request);
}
void WINAPI Secondary(void* self, int flags, int request)
{
    Record(AutoHideTraceKind::SecondaryUnhide, flags, request, _ReturnAddress());
    secondaryOriginal(self, flags, request);
}

LONG InstallAdapter() noexcept
{
    const HMODULE taskbarModule = GetModuleHandleW(L"Taskbar.dll");
    if (!IsSupportedImage(taskbarModule)) return -1;
    moduleBase.store(reinterpret_cast<std::uintptr_t>(taskbarModule), std::memory_order_release);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return -2;
    auto* base = reinterpret_cast<BYTE*>(taskbarModule);
    void* primary = base + 0x924c0;
    void* secondary = base + 0x14cca8;
    // Verify entry bytes as well as image identity before writing a trampoline.
    constexpr BYTE prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x18};
    if (std::memcmp(primary, prologue, sizeof(prologue)) != 0 ||
        std::memcmp(secondary, prologue, sizeof(prologue)) != 0) return -3;
    if (MH_CreateHook(primary, &Primary, reinterpret_cast<void**>(&primaryOriginal)) != MH_OK)
        return -4;
    if (MH_CreateHook(secondary, &Secondary, reinterpret_cast<void**>(&secondaryOriginal)) != MH_OK)
    {
        MH_RemoveHook(primary);
        return -5;
    }
    if (MH_EnableHook(primary) != MH_OK || MH_EnableHook(secondary) != MH_OK)
    {
        // Leave successfully created trampolines alive: a callback may already
        // be in flight. Disabled diagnostics always call the original function.
        MH_DisableHook(primary);
        MH_DisableHook(secondary);
        return -6;
    }
    return 1;
}
}

void Configure(AutoHideTraceBuffer* buffer, bool observe) noexcept
{
    if (!observe)
    {
        output.store(nullptr, std::memory_order_release);
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
    }
    catch (...) { output.store(nullptr, std::memory_order_release); }
}

LRESULT Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
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
