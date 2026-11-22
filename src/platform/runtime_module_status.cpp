#include "runtime_module_status.h"

#include <tlhelp32.h>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snowdesktop::runtime_modules
{
namespace
{
struct WindowIdentity
{
    HWND window;
    DWORD process;
    DWORD thread;
};

struct MinimizeTargets
{
    std::vector<WindowIdentity> administrator;
    std::vector<WindowIdentity> fullscreenSkipped;
};

std::mutex minimizeTargetsMutex;
MinimizeTargets minimizeTargets;

std::vector<WindowIdentity> CaptureWindows(std::span<const HWND> windows)
{
    std::vector<WindowIdentity> identities;
    identities.reserve(windows.size());
    for (const HWND window : windows)
    {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (thread && process) identities.push_back({window, process, thread});
    }
    return identities;
}

bool IsCurrent(const WindowIdentity& identity)
{
    DWORD process = 0;
    return GetWindowThreadProcessId(identity.window, &process) == identity.thread && process == identity.process;
}

Target DescribeMinimizeProcess(const PROCESSENTRY32W& process, bool administrator, bool resident, bool hook32)
{
    Target result{process.th32ProcessID, process.szExeFile, _wcsicmp(process.szExeFile, L"explorer.exe") == 0};
    result.hook32 = hook32;
    if (administrator) result.access = TargetAccess::Administrator;
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.process);
    if (!handle) return result;
    if (!resident)
    {
        BOOL wow64 = FALSE;
        result.hook32 = IsWow64Process(handle, &wow64) && wow64;
    }
    HANDLE token = nullptr;
    if (!administrator && OpenProcessToken(handle, TOKEN_QUERY, &token))
    {
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
            result.access = elevation.TokenIsElevated ? TargetAccess::Administrator : TargetAccess::Standard;
        CloseHandle(token);
    }
    CloseHandle(handle);
    return result;
}
}

void SetWindowMinimizeTargets(std::span<const HWND> administrator, std::span<const HWND> fullscreenSkipped)
{
    MinimizeTargets updated{CaptureWindows(administrator), CaptureWindows(fullscreenSkipped)};
    std::lock_guard lock(minimizeTargetsMutex);
    minimizeTargets = std::move(updated);
}

Snapshot Inspect(bool includeResidency)
{
    Snapshot result;
    result.policyRevision = policyRevision.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < kCount; ++i)
    {
        Status status;
        status.module = static_cast<Module>(i);
        status.allowed = Allowed(status.module);
        status.active = active[i].load(std::memory_order_acquire);
        status.error = lastError[i].load(std::memory_order_acquire);
        result.modules.push_back(std::move(status));
    }
    if (!includeResidency) return result;

    auto& minimize = result.modules[static_cast<std::size_t>(Module::WindowMinimize)];
    MinimizeTargets selected;
    if (minimize.allowed)
    {
        std::lock_guard lock(minimizeTargetsMutex);
        selected = minimizeTargets;
    }
    std::unordered_set<DWORD> administrators;
    for (const auto& identity : selected.administrator)
        if (IsCurrent(identity)) administrators.insert(identity.process);
    std::unordered_multimap<DWORD, HWND> skipped;
    for (const auto& identity : selected.fullscreenSkipped)
        if (IsCurrent(identity)) skipped.emplace(identity.process, identity.window);

    const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE)
    {
        for (auto& status : result.modules) status.residencyKnown = false;
        return result;
    }
    PROCESSENTRY32W process{sizeof(process)};
    if (Process32FirstW(processes, &process)) do
    {
        const DWORD pid = process.th32ProcessID;
        if (pid == GetCurrentProcessId() || !pid) continue;
        const auto [first, last] = skipped.equal_range(pid);
        for (auto it = first; it != last; ++it)
        {
            wchar_t title[256]{};
            GetWindowTextW(it->second, title, static_cast<int>(std::size(title)));
            minimize.fullscreenSkipped.push_back({pid, process.szExeFile, title});
        }
        const bool administrator = administrators.contains(pid);
        const bool explorer = _wcsicmp(process.szExeFile, L"explorer.exe") == 0;
        const bool start = _wcsicmp(process.szExeFile, L"StartMenuExperienceHost.exe") == 0;
        const bool wallpaper = _wcsnicmp(process.szExeFile, L"wallpaper", 9) == 0;
        const HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (modules == INVALID_HANDLE_VALUE)
        {
            // An inaccessible application could retain an old window module.
            minimize.residencyKnown = false;
            if (administrator) minimize.administratorTargets.push_back(DescribeMinimizeProcess(process, true, false, false));
            if (explorer) for (const auto i : {0, 1, 3}) result.modules[i].residencyKnown = false;
            if (start) result.modules[3].residencyKnown = false;
            if (wallpaper) result.modules[4].residencyKnown = false;
            continue;
        }
        bool taskbarImage = false, windowImage = false, wallpaperImage = false, entryImage = false;
        bool entry32 = false, window32 = false;
        MODULEENTRY32W module{sizeof(module)};
        if (Module32FirstW(modules, &module)) do
        {
            entryImage |= _wcsicmp(module.szModule, L"SnowDesktopHookEntry.dll") == 0;
            entry32 |= _wcsicmp(module.szModule, L"SnowDesktopHookEntry32.dll") == 0;
            taskbarImage |= _wcsicmp(module.szModule, L"SnowDesktopTaskbarHook.dll") == 0;
            windowImage |= _wcsicmp(module.szModule, L"SnowDesktopDockMinimizeHook.dll") == 0;
            window32 |= _wcsicmp(module.szModule, L"SnowDesktopDockMinimizeHook32.dll") == 0;
            wallpaperImage |= _wcsnicmp(module.szModule, L"SnowDesktopWallpaperHook", 24) == 0;
        } while (Module32NextW(modules, &module));
        CloseHandle(modules);
        entryImage |= entry32;
        const auto append = [&](std::size_t i) { result.modules[i].targets.push_back({pid, process.szExeFile, explorer}); };
        if ((taskbarImage || entryImage) && explorer) { append(0); append(1); append(3); }
        if ((taskbarImage || entryImage) && start) append(3);
        // The stable entry can outlive its payload after controlled cleanup.
        // In application processes it is also part of the window module residency.
        if (windowImage || window32 || (entryImage && !explorer && !start))
            minimize.targets.push_back(DescribeMinimizeProcess(process, administrator, true, window32 || entry32));
        else if (administrator)
            minimize.administratorTargets.push_back(DescribeMinimizeProcess(process, true, false, false));
        if (wallpaperImage) append(4);
    } while (Process32NextW(processes, &process));
    CloseHandle(processes);
    return result;
}
}
