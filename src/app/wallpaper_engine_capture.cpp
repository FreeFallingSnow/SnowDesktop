/**
 * @file wallpaper_engine_capture.cpp
 * @brief Request one clean Wallpaper Engine GPU frame for component previews.
 */
#include "wallpaper_engine_capture.h"

#include "../wallpaper_hook/wallpaper_hook_protocol.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop::wallpaper_engine_capture
{

CancellableWaitResult WaitForHandleOrCancellation(HANDLE handle,
    DWORD timeoutMs, const std::atomic_bool* cancelled)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return CancellableWaitResult::Failed;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        if (cancelled &&
            cancelled->load(std::memory_order_relaxed))
            return CancellableWaitResult::Cancelled;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            return CancellableWaitResult::TimedOut;
        const DWORD remaining = static_cast<DWORD>(
            std::min<ULONGLONG>(deadline - now, MAXDWORD));
        const DWORD slice = cancelled
            ? std::min<DWORD>(remaining, 20)
            : remaining;
        const DWORD result = WaitForSingleObject(handle, slice);
        if (result == WAIT_OBJECT_0)
            return CancellableWaitResult::Signaled;
        if (result == WAIT_FAILED)
            return CancellableWaitResult::Failed;
        if (result != WAIT_TIMEOUT)
            return CancellableWaitResult::Failed;
    }
}

namespace
{

using Microsoft::WRL::ComPtr;
using namespace snow::wallpaper_hook;

enum class ProcessArchitecture
{
    x86,
    x64,
};

struct ProcessEntry
{
    DWORD processId = 0;
    DWORD parentId = 0;
};

struct OutputWindow
{
    HWND window = nullptr;
    RECT desktopBounds{};
    DWORD ownerProcessId = 0;
};

std::wstring FormatSystemError(const wchar_t* stage, DWORD error)
{
    wchar_t message[192]{};
    swprintf_s(message, L"%s (error %lu)", stage,
        static_cast<unsigned long>(error));
    return message;
}

std::wstring FormatHresult(const wchar_t* stage, HRESULT result)
{
    wchar_t message[192]{};
    swprintf_s(message, L"%s (0x%08X)", stage,
        static_cast<unsigned>(result));
    return message;
}

bool IsCancelled(const std::atomic_bool* cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

std::int64_t IntersectionArea(const RECT& left, const RECT& right)
{
    RECT intersection{};
    if (!IntersectRect(&intersection, &left, &right))
        return 0;
    return static_cast<std::int64_t>(intersection.right - intersection.left) *
        static_cast<std::int64_t>(intersection.bottom - intersection.top);
}

std::wstring ProcessName(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, processId);
    if (!process)
        return {};
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(
        process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!queried)
        return {};
    std::wstring name = std::filesystem::path(path.data()).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return name;
}

bool IsWebRendererName(const std::wstring& name)
{
    return name == L"webwallpaper32.exe" ||
        name == L"webwallpaper64.exe" ||
        name == L"edgewallpaper32.exe" ||
        name == L"edgewallpaper64.exe" ||
        name == L"msedgewebview2.exe";
}

bool IsWallpaperEngineRendererName(const std::wstring& name)
{
    return name == L"wallpaper32.exe" ||
        name == L"wallpaper64.exe" || IsWebRendererName(name);
}

std::vector<ProcessEntry> ProcessSnapshot()
{
    std::vector<ProcessEntry> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            result.push_back({
                entry.th32ProcessID, entry.th32ParentProcessID });
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::wstring ProcessCommandLine(DWORD processId)
{
    using NtQueryInformationProcessFn = LONG(NTAPI*)(
        HANDLE, ULONG, PVOID, ULONG, PULONG);
    struct NativeUnicodeString
    {
        USHORT length;
        USHORT maximumLength;
        PWSTR buffer;
    };
    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
            "NtQueryInformationProcess"));
    if (!query)
        return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, processId);
    if (!process)
        return {};
    ULONG required = 0;
    query(process, 60, nullptr, 0, &required);
    if (required < sizeof(NativeUnicodeString))
    {
        CloseHandle(process);
        return {};
    }
    std::vector<std::byte> buffer(required);
    const LONG status = query(
        process, 60, buffer.data(), required, &required);
    CloseHandle(process);
    if (status < 0)
        return {};
    const auto* command =
        reinterpret_cast<const NativeUnicodeString*>(buffer.data());
    if (!command->buffer || !command->length)
        return {};
    std::wstring result(
        command->buffer, command->length / sizeof(wchar_t));
    std::transform(result.begin(), result.end(), result.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return result;
}

DWORD ResolveWebOwnerProcess(DWORD processId,
    const std::vector<ProcessEntry>& processes)
{
    if (!processId || !IsWebRendererName(ProcessName(processId)))
        return processId;
    DWORD owner = processId;
    for (int depth = 0; depth < 8; ++depth)
    {
        const auto current = std::find_if(
            processes.begin(), processes.end(),
            [owner](const ProcessEntry& entry) {
                return entry.processId == owner;
            });
        if (current == processes.end() || !current->parentId ||
            !IsWebRendererName(ProcessName(current->parentId)))
            break;
        owner = current->parentId;
    }
    return owner;
}

std::vector<DWORD> ResolveCaptureProcesses(DWORD ownerProcessId,
    const std::vector<ProcessEntry>& processes)
{
    ownerProcessId = ResolveWebOwnerProcess(ownerProcessId, processes);
    const std::wstring ownerName = ProcessName(ownerProcessId);
    if (!IsWebRendererName(ownerName))
        return IsWallpaperEngineRendererName(ownerName)
            ? std::vector<DWORD>{ ownerProcessId }
            : std::vector<DWORD>{};

    std::unordered_set<DWORD> descendants{ ownerProcessId };
    for (int pass = 0; pass < 6; ++pass)
    {
        for (const ProcessEntry& process : processes)
        {
            if (descendants.contains(process.parentId))
                descendants.insert(process.processId);
        }
    }

    std::vector<DWORD> gpuProcesses;
    for (DWORD processId : descendants)
    {
        if (processId == ownerProcessId ||
            !IsWebRendererName(ProcessName(processId)))
            continue;
        if (ProcessCommandLine(processId).find(L"--type=gpu-process") !=
            std::wstring::npos)
            gpuProcesses.push_back(processId);
    }
    if (gpuProcesses.empty())
        gpuProcesses.push_back(ownerProcessId);
    return gpuProcesses;
}

bool IsOutputWindowClass(HWND window)
{
    std::array<wchar_t, 64> className{};
    if (!window || !GetClassNameW(
            window, className.data(), static_cast<int>(className.size())))
        return false;
    return _wcsicmp(className.data(), L"WPEDesktopDX11Window") == 0 ||
        _wcsicmp(className.data(), L"WPEDesktopCEFWindow") == 0 ||
        _wcsicmp(className.data(), L"WPEVideoWallpaper") == 0 ||
        _wcsicmp(className.data(), L"WPECloneView") == 0 ||
        _wcsicmp(className.data(), L"EVRFullscreenVideo") == 0 ||
        _wcsicmp(className.data(), L"Intermediate D3D Window") == 0;
}

int OutputWindowPriority(HWND window)
{
    std::array<wchar_t, 64> className{};
    GetClassNameW(window, className.data(),
        static_cast<int>(className.size()));
    if (_wcsicmp(className.data(), L"WPEDesktopDX11Window") == 0 ||
        _wcsicmp(className.data(), L"WPEVideoWallpaper") == 0)
        return 4;
    if (_wcsicmp(className.data(), L"WPECloneView") == 0 ||
        _wcsicmp(className.data(), L"EVRFullscreenVideo") == 0 ||
        _wcsicmp(className.data(), L"Intermediate D3D Window") == 0)
        return 3;
    return 1;
}

bool IsCloneOutputWindow(HWND window)
{
    std::array<wchar_t, 64> className{};
    return window && GetClassNameW(window, className.data(),
            static_cast<int>(className.size())) &&
        _wcsicmp(className.data(), L"WPECloneView") == 0;
}

std::vector<OutputWindow> DiscoverOutputWindows(const RECT& monitorBounds,
    const std::vector<ProcessEntry>& processes)
{
    std::vector<HWND> desktopHosts;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* hosts = reinterpret_cast<std::vector<HWND>*>(parameter);
        std::array<wchar_t, 64> className{};
        if (GetClassNameW(window, className.data(),
                static_cast<int>(className.size())) &&
            (_wcsicmp(className.data(), L"WorkerW") == 0 ||
                _wcsicmp(className.data(), L"Progman") == 0))
            hosts->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&desktopHosts));

    struct EnumerationContext
    {
        const RECT* monitorBounds = nullptr;
        const std::vector<ProcessEntry>* processes = nullptr;
        std::vector<OutputWindow>* outputs = nullptr;
    };
    EnumerationContext context{
        &monitorBounds, &processes, nullptr };
    std::vector<OutputWindow> outputs;
    context.outputs = &outputs;
    for (HWND host : desktopHosts)
    {
        EnumChildWindows(host, [](HWND window, LPARAM parameter) -> BOOL {
            auto* context = reinterpret_cast<EnumerationContext*>(parameter);
            if (!IsWindowVisible(window) || !IsOutputWindowClass(window))
                return TRUE;
            RECT bounds{};
            if (!GetWindowRect(window, &bounds) ||
                IntersectionArea(bounds, *context->monitorBounds) <= 0)
                return TRUE;
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            processId = ResolveWebOwnerProcess(
                processId, *context->processes);
            if (!IsWallpaperEngineRendererName(ProcessName(processId)))
                return TRUE;
            const auto duplicate = std::find_if(
                context->outputs->begin(), context->outputs->end(),
                [window](const OutputWindow& output) {
                    return output.window == window;
                });
            if (duplicate == context->outputs->end())
                context->outputs->push_back({ window, bounds, processId });
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
    }
    return outputs;
}

bool QueryProcessArchitecture(DWORD processId,
    ProcessArchitecture& architecture, std::wstring& error)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, processId);
    if (!process)
    {
        error = FormatSystemError(
            L"Cannot query Wallpaper Engine process", GetLastError());
        return false;
    }
    const std::wstring name = ProcessName(processId);
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
            "IsWow64Process2"));
    bool known = false;
    if (isWow64Process2)
    {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (isWow64Process2(process, &processMachine, &nativeMachine))
        {
            if (processMachine == IMAGE_FILE_MACHINE_I386)
            {
                architecture = ProcessArchitecture::x86;
                known = true;
            }
            else if (processMachine == IMAGE_FILE_MACHINE_AMD64 ||
                (processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
                    nativeMachine == IMAGE_FILE_MACHINE_AMD64))
            {
                architecture = ProcessArchitecture::x64;
                known = true;
            }
        }
    }
    else
    {
        BOOL wow64 = FALSE;
        if (IsWow64Process(process, &wow64))
        {
            architecture = wow64
                ? ProcessArchitecture::x86
                : ProcessArchitecture::x64;
            known = true;
        }
    }
    CloseHandle(process);
    if (!known)
    {
        error = L"Unsupported Wallpaper Engine process architecture";
        return false;
    }
    const bool nameMatchesArchitecture =
        architecture == ProcessArchitecture::x86
        ? name.ends_with(L"32.exe") || name == L"msedgewebview2.exe"
        : name.ends_with(L"64.exe") || name == L"msedgewebview2.exe";
    if (!IsWallpaperEngineRendererName(name) || !nameMatchesArchitecture)
    {
        error = L"Target is not a supported Wallpaper Engine renderer";
        return false;
    }
    return true;
}

std::filesystem::path ApplicationDirectory()
{
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
        static_cast<DWORD>(modulePath.size()));
    return length && length < modulePath.size()
        ? std::filesystem::path(modulePath.data()).parent_path()
        : std::filesystem::path{};
}

std::filesystem::path RuntimeFilePath(const wchar_t* filename)
{
    const std::filesystem::path directory = ApplicationDirectory();
    if (directory.empty())
        return {};
    const std::filesystem::path runtime =
        directory / L"SnowDesktop.Runtime" / filename;
    return std::filesystem::is_regular_file(runtime)
        ? runtime
        : directory / filename;
}

std::filesystem::path HookPath(ProcessArchitecture architecture)
{
    return RuntimeFilePath(
        architecture == ProcessArchitecture::x86
            ? L"SnowDesktopWallpaperHook32.dll"
            : L"SnowDesktopWallpaperHook.dll");
}

void ReapRemoteInjection(HANDLE process, HANDLE thread, void* remotePath)
{
    try
    {
        std::thread([process, thread, remotePath] {
            WaitForSingleObject(thread, INFINITE);
            if (remotePath)
                VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            CloseHandle(thread);
            CloseHandle(process);
        }).detach();
    }
    catch (...)
    {
        CloseHandle(thread);
        CloseHandle(process);
    }
}

void ReapProcess(HANDLE process)
{
    try
    {
        std::thread([process] {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
        }).detach();
    }
    catch (...)
    {
        CloseHandle(process);
    }
}

bool InjectDirect(DWORD processId, const std::filesystem::path& dllPath,
    DWORD waitMs, const std::atomic_bool* cancelled,
    std::wstring& error)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId);
    if (!process)
    {
        error = FormatSystemError(
            L"Cannot open Wallpaper Engine process", GetLastError());
        return false;
    }
    const std::wstring path = std::filesystem::absolute(dllPath).wstring();
    const SIZE_T pathBytes = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, pathBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    SIZE_T written = 0;
    const bool wrotePath = remotePath && WriteProcessMemory(process,
        remotePath, path.c_str(), pathBytes, &written) &&
        written == pathBytes;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr);
    HANDLE thread = wrotePath && loadLibrary
        ? CreateRemoteThread(process, nullptr, 0,
            loadLibrary, remotePath, 0, nullptr)
        : nullptr;
    if (!thread)
    {
        error = FormatSystemError(
            L"Wallpaper Engine hook injection failed", GetLastError());
        if (remotePath)
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    const CancellableWaitResult waitResult =
        WaitForHandleOrCancellation(thread, waitMs, cancelled);
    DWORD remoteModule = 0;
    const bool loaded = waitResult == CancellableWaitResult::Signaled &&
        GetExitCodeThread(thread, &remoteModule) && remoteModule != 0;
    if (waitResult == CancellableWaitResult::Signaled)
    {
        CloseHandle(thread);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
    }
    else
    {
        ReapRemoteInjection(process, thread, remotePath);
    }
    if (!loaded)
    {
        if (waitResult == CancellableWaitResult::Cancelled)
            error = L"Wallpaper Engine hook injection was cancelled";
        else if (waitResult == CancellableWaitResult::TimedOut)
            error = L"Wallpaper Engine hook injection timed out";
        else
            error = L"Wallpaper Engine hook DLL did not load";
        return false;
    }
    return true;
}

bool Inject32(DWORD processId, const std::filesystem::path& dllPath,
    DWORD waitMs, const std::atomic_bool* cancelled,
    std::wstring& error)
{
    const std::filesystem::path injector =
        RuntimeFilePath(L"SnowDesktopWallpaperInjector32.exe");
    if (!std::filesystem::is_regular_file(injector))
    {
        error = L"SnowDesktopWallpaperInjector32.exe is missing";
        return false;
    }
    std::wstring commandLine = L"\"" + injector.wstring() + L"\" " +
        std::to_wstring(processId) + L" \"" +
        std::filesystem::absolute(dllPath).wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(injector.c_str(), mutableCommand.data(), nullptr,
            nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            injector.parent_path().c_str(), &startup, &process))
    {
        error = FormatSystemError(
            L"Cannot start 32-bit Wallpaper Engine injector",
            GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    const CancellableWaitResult waitResult =
        WaitForHandleOrCancellation(
            process.hProcess, waitMs, cancelled);
    DWORD exitCode = STILL_ACTIVE;
    const bool completed =
        waitResult == CancellableWaitResult::Signaled &&
        GetExitCodeProcess(process.hProcess, &exitCode);
    if (!completed && waitResult == CancellableWaitResult::TimedOut)
    {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
    }
    else if (!completed)
    {
        ReapProcess(process.hProcess);
    }
    else
    {
        CloseHandle(process.hProcess);
    }
    if (!completed || exitCode != 0)
    {
        if (waitResult == CancellableWaitResult::Cancelled)
            error = L"32-bit Wallpaper Engine hook injection was cancelled";
        else if (!completed)
            error = L"32-bit Wallpaper Engine hook injection timed out";
        else
            error = L"32-bit Wallpaper Engine hook injection failed (code " +
                std::to_wstring(exitCode) + L")";
        return false;
    }
    return true;
}

ComPtr<IDXGIAdapter1> FindAdapter(const LUID& adapterLuid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            description.AdapterLuid.LowPart == adapterLuid.LowPart &&
            description.AdapterLuid.HighPart == adapterLuid.HighPart)
            return adapter;
    }
    return {};
}

std::uint32_t ConvertPixel(std::uint32_t value, DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return value | 0xff000000u;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    {
        const std::uint32_t red = value & 0xffu;
        const std::uint32_t green = (value >> 8) & 0xffu;
        const std::uint32_t blue = (value >> 16) & 0xffu;
        return 0xff000000u | (red << 16) | (green << 8) | blue;
    }
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    {
        const std::uint32_t red =
            ((value & 0x3ffu) * 255u + 511u) / 1023u;
        const std::uint32_t green =
            (((value >> 10) & 0x3ffu) * 255u + 511u) / 1023u;
        const std::uint32_t blue =
            (((value >> 20) & 0x3ffu) * 255u + 511u) / 1023u;
        return 0xff000000u | (red << 16) | (green << 8) | blue;
    }
    default:
        return 0;
    }
}

bool IsSupportedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

class CaptureSession
{
public:
    explicit CaptureSession(DWORD processId)
        : processId_(processId)
    {
    }

    ~CaptureSession()
    {
        Shutdown();
    }

    bool Start(DWORD waitMs, const std::atomic_bool* cancelled,
        std::wstring& error)
    {
        if (IsCancelled(cancelled))
            return false;
        if (!OpenMapping())
        {
            ProcessArchitecture architecture{};
            if (!QueryProcessArchitecture(processId_, architecture, error))
                return false;
            const std::filesystem::path hookPath = HookPath(architecture);
            if (hookPath.empty() ||
                !std::filesystem::is_regular_file(hookPath))
            {
                error = architecture == ProcessArchitecture::x86
                    ? L"SnowDesktopWallpaperHook32.dll is missing"
                    : L"SnowDesktopWallpaperHook.dll is missing";
                return false;
            }
            const bool injected = architecture == ProcessArchitecture::x86
                ? Inject32(processId_, hookPath, waitMs, cancelled, error)
                : InjectDirect(processId_, hookPath, waitMs, cancelled, error);
            if (!injected)
                return false;
            const ULONGLONG deadline = GetTickCount64() + waitMs;
            while (!IsCancelled(cancelled) && !OpenMapping() &&
                GetTickCount64() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!state_)
        {
            error = L"Wallpaper Engine hook mapping was not published";
            return false;
        }
        if (state_->status == static_cast<LONG>(Status::failed))
        {
            error = FormatHresult(L"Wallpaper Engine hook initialization failed",
                static_cast<HRESULT>(state_->last_hresult));
            return false;
        }
        InterlockedExchange(&state_->shutdown_requested, 0);
        InterlockedExchange(&state_->requested_interval_ms, 0);
        InterlockedExchange(&state_->capture_enabled, 1);
        InterlockedExchange64(&state_->consumer_heartbeat,
            static_cast<LONG64>(GetTickCount64()));
        requestSerial_ = InterlockedIncrement(&state_->request_serial);
        return true;
    }

    bool TryReadBestFrame(const RECT& monitorBounds,
        bool allowCloneRemap, Backdrop& backdrop, std::wstring& error)
    {
        if (!state_)
            return false;
        InterlockedExchange64(&state_->consumer_heartbeat,
            static_cast<LONG64>(GetTickCount64()));
        if (state_->status == static_cast<LONG>(Status::failed))
        {
            error = FormatHresult(L"Wallpaper Engine hook failed",
                static_cast<HRESULT>(state_->last_hresult));
            return false;
        }

        struct Candidate
        {
            std::size_t index = 0;
            std::int64_t score = 0;
            bool remapToMonitor = false;
        };
        std::vector<Candidate> candidates;
        const LONG observedCount = state_->slot_count;
        const LONG count = std::clamp<LONG>(observedCount, 0,
            static_cast<LONG>(kMaxFrameSlots));
        for (LONG rawIndex = 0; rawIndex < count; ++rawIndex)
        {
            const std::size_t index = static_cast<std::size_t>(rawIndex);
            const SharedFrameSlot& slot = state_->slots[index];
            if (!slot.swap_chain || !slot.shared_handle ||
                slot.completed_request_serial != requestSerial_ ||
                slot.frame_number <= 0)
                continue;
            const std::int64_t area =
                IntersectionArea(slot.desktop_rect, monitorBounds);
            const bool remapToMonitor = area <= 0 && allowCloneRemap;
            if (area <= 0 && !remapToMonitor)
                continue;
            candidates.push_back({ index,
                std::max<std::int64_t>(area, 0) * 8 + OutputWindowPriority(
                    reinterpret_cast<HWND>(slot.output_window)),
                remapToMonitor });
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                return left.score > right.score;
            });
        for (const Candidate& candidate : candidates)
        {
            if (ReadFrame(candidate.index, monitorBounds,
                    candidate.remapToMonitor, backdrop, error))
                return true;
        }
        return false;
    }

private:
    bool OpenMapping()
    {
        if (state_)
            return true;
        wchar_t mappingName[128]{};
        MakeMappingName(processId_, mappingName);
        mapping_ = OpenFileMappingW(
            FILE_MAP_ALL_ACCESS, FALSE, mappingName);
        if (!mapping_)
            return false;
        state_ = static_cast<SharedState*>(MapViewOfFile(mapping_,
            FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
        if (!state_ || state_->magic != kMagic ||
            state_->version != kVersion ||
            state_->process_id != processId_)
        {
            CloseMapping();
            return false;
        }
        return true;
    }

    void CloseMapping()
    {
        if (state_)
            UnmapViewOfFile(state_);
        if (mapping_)
            CloseHandle(mapping_);
        state_ = nullptr;
        mapping_ = nullptr;
    }

    void Shutdown()
    {
        if (state_ && state_->magic == kMagic && state_->version == kVersion)
        {
            InterlockedExchange(&state_->capture_enabled, 0);
            InterlockedExchange64(&state_->consumer_heartbeat, 0);
            InterlockedExchange(&state_->shutdown_requested, 1);
        }
        CloseMapping();
    }

    bool ReadFrame(std::size_t index, const RECT& monitorBounds,
        bool remapToMonitor, Backdrop& backdrop, std::wstring& error)
    {
        SharedFrameSlot& published = state_->slots[index];
        const DXGI_FORMAT format =
            static_cast<DXGI_FORMAT>(published.format);
        if (!IsSupportedFormat(format))
        {
            error = L"Wallpaper Engine frame uses an unsupported DXGI format";
            return false;
        }
        const LUID adapterLuid{
            published.adapter_luid_low, published.adapter_luid_high };
        ComPtr<IDXGIAdapter1> adapter = FindAdapter(adapterLuid);
        if (!adapter)
        {
            error = L"Wallpaper Engine GPU adapter is unavailable";
            return false;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT result = D3D11CreateDevice(adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                D3D11_CREATE_DEVICE_SINGLETHREADED,
            nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context);
        if (FAILED(result))
        {
            error = FormatHresult(
                L"Cannot create Wallpaper Engine readback device", result);
            return false;
        }
        ComPtr<ID3D11Texture2D> source;
        result = device->OpenSharedResource(
            reinterpret_cast<HANDLE>(published.shared_handle),
            IID_PPV_ARGS(&source));
        if (FAILED(result) || !source)
        {
            error = FormatHresult(
                L"Cannot open Wallpaper Engine shared texture", result);
            return false;
        }
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        source.As(&keyedMutex);
        if (keyedMutex)
        {
            result = keyedMutex->AcquireSync(1, 50);
            if (result == WAIT_TIMEOUT)
                return false;
            if (FAILED(result))
            {
                error = FormatHresult(
                    L"Cannot acquire Wallpaper Engine frame", result);
                return false;
            }
        }
        const auto releaseMutex = [&] {
            if (keyedMutex)
                keyedMutex->ReleaseSync(0);
        };

        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        D3D11_TEXTURE2D_DESC stagingDescription = description;
        stagingDescription.BindFlags = 0;
        stagingDescription.MiscFlags = 0;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        result = device->CreateTexture2D(
            &stagingDescription, nullptr, &staging);
        if (FAILED(result))
        {
            releaseMutex();
            error = FormatHresult(
                L"Cannot create Wallpaper Engine staging texture", result);
            return false;
        }
        context->CopyResource(staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        result = context->Map(
            staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result))
        {
            releaseMutex();
            error = FormatHresult(
                L"Cannot map Wallpaper Engine staging texture", result);
            return false;
        }

        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(description.Width) * description.Height);
        for (UINT y = 0; y < description.Height; ++y)
        {
            const auto* sourceRow =
                reinterpret_cast<const std::uint32_t*>(
                    static_cast<const std::byte*>(mapped.pData) +
                    static_cast<std::size_t>(y) * mapped.RowPitch);
            auto* destinationRow = pixels.data() +
                static_cast<std::size_t>(y) * description.Width;
            for (UINT x = 0; x < description.Width; ++x)
                destinationRow[x] = ConvertPixel(sourceRow[x], format);
        }
        context->Unmap(staging.Get(), 0);
        releaseMutex();
        InterlockedExchange64(&published.consumed_frame_number,
            published.frame_number);

        const RECT sourceDesktopBounds = remapToMonitor
            ? monitorBounds
            : published.desktop_rect;
        backdrop = CropFrameToDesktopRegion(pixels.data(),
            static_cast<int>(description.Width),
            static_cast<int>(description.Height),
            sourceDesktopBounds, monitorBounds);
        if (backdrop.Empty())
        {
            error = L"Wallpaper Engine frame does not cover the preview monitor";
            return false;
        }
        return true;
    }

    DWORD processId_ = 0;
    HANDLE mapping_ = nullptr;
    SharedState* state_ = nullptr;
    LONG requestSerial_ = 0;
};

} // namespace

Backdrop CropFrameToDesktopRegion(const std::uint32_t* sourcePixels,
    int sourceWidth, int sourceHeight, const RECT& sourceDesktopBounds,
    const RECT& requestedDesktopBounds)
{
    Backdrop result;
    const int sourceDesktopWidth =
        sourceDesktopBounds.right - sourceDesktopBounds.left;
    const int sourceDesktopHeight =
        sourceDesktopBounds.bottom - sourceDesktopBounds.top;
    if (!sourcePixels || sourceWidth <= 0 || sourceHeight <= 0 ||
        sourceDesktopWidth <= 0 || sourceDesktopHeight <= 0)
        return result;
    if (!IntersectRect(&result.desktopBounds,
            &sourceDesktopBounds, &requestedDesktopBounds))
        return result;
    result.width = result.desktopBounds.right - result.desktopBounds.left;
    result.height = result.desktopBounds.bottom - result.desktopBounds.top;
    if (result.width <= 0 || result.height <= 0)
        return {};
    result.pixels.resize(
        static_cast<std::size_t>(result.width) * result.height);
    for (int y = 0; y < result.height; ++y)
    {
        const std::int64_t desktopY =
            static_cast<std::int64_t>(result.desktopBounds.top -
                sourceDesktopBounds.top + y) * sourceHeight;
        const int sourceY = std::clamp(
            static_cast<int>(desktopY / sourceDesktopHeight),
            0, sourceHeight - 1);
        for (int x = 0; x < result.width; ++x)
        {
            const std::int64_t desktopX =
                static_cast<std::int64_t>(result.desktopBounds.left -
                    sourceDesktopBounds.left + x) * sourceWidth;
            const int sourceX = std::clamp(
                static_cast<int>(desktopX / sourceDesktopWidth),
                0, sourceWidth - 1);
            result.pixels[static_cast<std::size_t>(y) * result.width + x] =
                sourcePixels[static_cast<std::size_t>(sourceY) *
                    sourceWidth + sourceX] | 0xff000000u;
        }
    }
    return result;
}

Result CaptureOneShotForMonitor(const RECT& monitorBounds,
    DWORD timeoutMs, const std::atomic_bool* cancelled)
{
    Result result;
    const int monitorWidth = monitorBounds.right - monitorBounds.left;
    const int monitorHeight = monitorBounds.bottom - monitorBounds.top;
    if (monitorWidth <= 0 || monitorHeight <= 0 || timeoutMs == 0)
    {
        result.error = L"Invalid Wallpaper Engine capture bounds";
        return result;
    }
    const std::vector<ProcessEntry> processes = ProcessSnapshot();
    const std::vector<OutputWindow> outputs =
        DiscoverOutputWindows(monitorBounds, processes);
    if (outputs.empty())
        return result;
    result.wallpaperEngineDetected = true;
    const bool allowCloneRemap = std::any_of(
        outputs.begin(), outputs.end(),
        [](const OutputWindow& output) {
            return IsCloneOutputWindow(output.window);
        });

    std::vector<DWORD> captureProcesses;
    for (const OutputWindow& output : outputs)
    {
        const std::vector<DWORD> resolved =
            ResolveCaptureProcesses(output.ownerProcessId, processes);
        for (DWORD processId : resolved)
        {
            if (processId && std::find(captureProcesses.begin(),
                    captureProcesses.end(), processId) ==
                    captureProcesses.end())
                captureProcesses.push_back(processId);
        }
    }
    if (captureProcesses.empty())
    {
        result.error = L"Wallpaper Engine renderer process was not found";
        return result;
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (DWORD processId : captureProcesses)
    {
        if (IsCancelled(cancelled) || GetTickCount64() >= deadline)
            break;
        CaptureSession session(processId);
        const ULONGLONG remaining = deadline - GetTickCount64();
        const DWORD startupWait = static_cast<DWORD>(
            std::clamp<ULONGLONG>(remaining, 250, 5000));
        std::wstring sessionError;
        if (!session.Start(startupWait, cancelled, sessionError))
        {
            if (!sessionError.empty())
                result.error = std::move(sessionError);
            continue;
        }
        while (!IsCancelled(cancelled) && GetTickCount64() < deadline)
        {
            if (session.TryReadBestFrame(monitorBounds,
                    allowCloneRemap, result.backdrop, sessionError))
            {
                result.error.clear();
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
        if (!sessionError.empty())
            result.error = std::move(sessionError);
    }
    if (result.error.empty() && !IsCancelled(cancelled))
        result.error = L"Timed out waiting for Wallpaper Engine Present";
    return result;
}

} // namespace snowdesktop::wallpaper_engine_capture
