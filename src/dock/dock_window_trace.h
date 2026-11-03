#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>

// Private, opt-in investigation recorder shared by the host, helper and injected
// x86/x64 hook. No formatting, symbol lookup, file append or message send occurs
// in a hook. Records occupy unique slots in a file-backed mapping; exhaustion
// is explicit and never overwrites the first failure.
namespace snowdesktop::dock_window_trace
{
inline constexpr wchar_t HostProperty[] = L"SnowDesktop.Dock.Trace.Host.v1";
inline constexpr wchar_t SessionProperty[] = L"SnowDesktop.Dock.Trace.Session.v1";
inline constexpr wchar_t RestoreProperty[] = L"SnowDesktop.Dock.Trace.Restore.v1";
inline constexpr wchar_t WindowRestoreProperty[] = L"SnowDesktop.Dock.Trace.WindowRestore.v1";
inline constexpr std::uint32_t Magic = 0x53445431;
inline constexpr LONG Capacity = 8192;

struct alignas(8) Header
{
    std::uint32_t magic, version, recordSize, capacity;
    volatile LONG next, dropped, restoreSerial;
    std::uint32_t skipCloak;
    std::uint64_t frequency, startTick;
    std::uint32_t hostProcess, reserved[3];
};
struct alignas(8) Record
{
    volatile LONG ready;
    std::uint32_t command;
    std::uint64_t counter, tick, window, foreground;
    std::uint32_t process, thread, targetProcess, targetThread;
    std::uint32_t restore, windowRestore, iconic, visible;
    std::uint32_t cloaked, cloakResult, showCommand, placementFlags, stackCount, result;
    char phase[48];
    std::uint64_t stack[32];
};
static_assert(sizeof(Header) == 64 && sizeof(Record) == 400);
inline constexpr DWORD MappingBytes = sizeof(Header) + Capacity * sizeof(Record);

inline void MappingName(DWORD process, wchar_t (&name)[96]) noexcept
{
    swprintf_s(name, L"Local\\SnowDesktop.Dock.Trace.v1.%lu", process);
}
struct View
{
    HANDLE mapping = nullptr;
    Header* header = nullptr;
    explicit View(HWND window) noexcept
    {
        const DWORD process = HandleToULong(GetPropW(window, HostProperty));
        if (!process) return;
        wchar_t name[96]{};
        MappingName(process, name);
        mapping = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, name);
        if (mapping) header = static_cast<Header*>(MapViewOfFile(mapping,
            FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, MappingBytes));
        if (header && (header->magic != Magic || header->version != 1 ||
            header->recordSize != sizeof(Record) || header->capacity != Capacity ||
            header->hostProcess != process || GetPropW(window, SessionProperty) !=
                ULongToHandle(static_cast<ULONG>(header->startTick) | 1u)))
        { UnmapViewOfFile(header); header = nullptr; }
    }
    ~View() { if (header) UnmapViewOfFile(header); if (mapping) CloseHandle(mapping); }
    View(const View&) = delete;
    View& operator=(const View&) = delete;
};

inline void Write(HWND window, const char* phase, UINT command = 0,
    bool captureStack = false, DWORD result = 0,
    ULONG restore = MAXDWORD, ULONG windowRestore = MAXDWORD) noexcept
{
    const DWORD lastError = GetLastError();
    {
        View view(window);
        if (view.header)
        {
            const LONG index = InterlockedIncrement(&view.header->next) - 1;
            if (index >= 0 && index < Capacity)
            {
                auto& record = reinterpret_cast<Record*>(view.header + 1)[index];
                LARGE_INTEGER counter{};
                QueryPerformanceCounter(&counter);
                record.counter = static_cast<std::uint64_t>(counter.QuadPart);
                record.tick = GetTickCount64();
                record.window = reinterpret_cast<UINT_PTR>(window);
                record.foreground = reinterpret_cast<UINT_PTR>(GetForegroundWindow());
                record.process = GetCurrentProcessId(); record.thread = GetCurrentThreadId();
                DWORD targetProcess = 0;
                record.targetThread = GetWindowThreadProcessId(window, &targetProcess);
                record.targetProcess = targetProcess;
                record.restore = restore != MAXDWORD ? restore : HandleToULong(GetPropW(window, RestoreProperty));
                record.windowRestore = windowRestore != MAXDWORD ? windowRestore :
                    HandleToULong(GetPropW(window, WindowRestoreProperty));
                record.iconic = IsIconic(window) != FALSE;
                record.visible = IsWindowVisible(window) != FALSE;
                DWORD cloaked = 0;
                record.cloakResult = static_cast<DWORD>(DwmGetWindowAttribute(
                    window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)));
                record.cloaked = cloaked;
                WINDOWPLACEMENT placement{sizeof(placement)};
                record.showCommand = GetWindowPlacement(window, &placement) ? placement.showCmd : MAXDWORD;
                record.placementFlags = placement.flags;
                record.command = command; record.result = result;
                strncpy_s(record.phase, phase, _TRUNCATE);
                if (captureStack)
                {
                    void* frames[32]{};
                    record.stackCount = CaptureStackBackTrace(1, 32, frames, nullptr);
                    for (DWORD frame = 0; frame < record.stackCount; ++frame)
                        record.stack[frame] = reinterpret_cast<UINT_PTR>(frames[frame]);
                }
                InterlockedExchange(&record.ready, 1);
            }
            else InterlockedIncrement(&view.header->dropped);
        }
    }
    SetLastError(lastError);
}

inline bool SkipCloak(HWND window) noexcept
{
    const DWORD lastError = GetLastError();
    bool skip = false;
    { View view(window); skip = view.header && view.header->skipCloak != 0; }
    SetLastError(lastError);
    return skip;
}

// Only the regular host calls Begin. Enable by placing dock-window-trace.txt
// beside SnowDesktop.exe containing "observe" or "skip-cloak". The mode is
// frozen for this process. Delete the marker and restart to disable recording.
// Binary evidence is independent of the Dock/Taskbar log toggles and sessions.
class Host final
{
public:
    void Begin(HWND window, bool restore) noexcept
    {
        const DWORD lastError = GetLastError();
        {
            std::lock_guard lock(mutex_);
            if (!attempted_) { attempted_ = true; Initialize(); }
            if (header_)
            {
                const auto process = GetCurrentProcessId();
                const HANDLE session = ULongToHandle(static_cast<ULONG>(header_->startTick) | 1u);
                const bool sameHost = GetPropW(window, HostProperty) == ULongToHandle(process) &&
                    GetPropW(window, SessionProperty) == session;
                if (!sameHost)
                {
                    RemovePropW(window, RestoreProperty);
                    RemovePropW(window, WindowRestoreProperty);
                }
                if (SetPropW(window, HostProperty, ULongToHandle(process)) &&
                    SetPropW(window, SessionProperty, session))
                {
                    if (restore)
                    {
                        const ULONG ordinal = static_cast<ULONG>(InterlockedIncrement(&header_->restoreSerial));
                        const ULONG windowOrdinal = HandleToULong(GetPropW(window, WindowRestoreProperty)) + 1;
                        if (!SetPropW(window, RestoreProperty, ULongToHandle(ordinal)) ||
                            !SetPropW(window, WindowRestoreProperty, ULongToHandle(windowOrdinal)))
                            InterlockedIncrement(&header_->dropped);
                    }
                    Write(window, restore ? "restore-begin" : "host-minimize-intent");
                }
                else InterlockedIncrement(&header_->dropped);
            }
        }
        SetLastError(lastError);
    }
    ~Host()
    {
        if (header_) { FlushViewOfFile(header_, 0); UnmapViewOfFile(header_); }
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
private:
    void Initialize() noexcept
    {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return;
        auto* slash = wcsrchr(path, L'\\');
        if (!slash) return;
        const std::size_t prefix = static_cast<std::size_t>(slash + 1 - path);
        if (prefix + 128 >= ARRAYSIZE(path)) return;
        wcscpy_s(path + prefix, ARRAYSIZE(path) - prefix, L"dock-window-trace.txt");
        HANDLE marker = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker == INVALID_HANDLE_VALUE) return;
        char mode[32]{}; DWORD read = 0;
        const BOOL loaded = ReadFile(marker, mode, sizeof(mode) - 1, &read, nullptr);
        CloseHandle(marker);
        const bool skip = loaded && std::strncmp(mode, "skip-cloak", 10) == 0;
        if (!loaded || (!skip && std::strncmp(mode, "observe", 7) != 0)) return;
        wcscpy_s(path + prefix, ARRAYSIZE(path) - prefix, L"dock-window-traces");
        if (!CreateDirectoryW(path, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
        swprintf_s(path + wcslen(path), ARRAYSIZE(path) - wcslen(path), L"\\%lu-%llu.bin",
            GetCurrentProcessId(), GetTickCount64());
        file_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        wchar_t name[96]{}; MappingName(GetCurrentProcessId(), name);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, MappingBytes, name);
        if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) return;
        header_ = static_cast<Header*>(MapViewOfFile(mapping_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, MappingBytes));
        if (!header_) return;
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        header_->version = 1; header_->recordSize = sizeof(Record); header_->capacity = Capacity;
        header_->skipCloak = skip ? 1u : 0u;
        header_->frequency = static_cast<std::uint64_t>(frequency.QuadPart);
        header_->startTick = GetTickCount64(); header_->hostProcess = GetCurrentProcessId();
        MemoryBarrier(); header_->magic = Magic;
    }
    std::mutex mutex_;
    bool attempted_ = false;
    HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
    Header* header_ = nullptr;
};
inline void Begin(HWND window, bool restore) noexcept
{
    static Host host;
    host.Begin(window, restore);
}
}
