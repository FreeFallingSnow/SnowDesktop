#pragma once
#include "runtime_module_policy.h"
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

namespace snowdesktop::runtime_modules
{
struct Target
{
    std::uint32_t process = 0;
    std::wstring name;
    bool explorer = false;
};
struct Status
{
    Module module = Module::Taskbar;
    bool allowed = true;
    unsigned active = 0;
    std::uint32_t error = 0;
    bool residencyKnown = true;
    std::vector<Target> targets;
};
struct Snapshot
{
    std::uint64_t policyRevision = 0;
    std::vector<Status> modules;
};
// Never connects a controller, loads a DLL or sends an attach message.
inline Snapshot Inspect()
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
    const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE)
    {
        for (auto& status : result.modules) status.residencyKnown = false;
        return result;
    }
    PROCESSENTRY32W process{sizeof(process)};
    if (Process32FirstW(processes, &process)) do
    {
        if (process.th32ProcessID == GetCurrentProcessId() || !process.th32ProcessID) continue;
        const bool explorer = _wcsicmp(process.szExeFile, L"explorer.exe") == 0;
        const bool start = _wcsicmp(process.szExeFile, L"StartMenuExperienceHost.exe") == 0;
        const bool wallpaper = _wcsnicmp(process.szExeFile, L"wallpaper", 9) == 0;
        const HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process.th32ProcessID);
        if (modules == INVALID_HANDLE_VALUE)
        {
            // An inaccessible application could retain an old window module.
            result.modules[2].residencyKnown = false;
            if (explorer) for (const auto i : {0, 1, 3}) result.modules[i].residencyKnown = false;
            if (start) result.modules[3].residencyKnown = false;
            if (wallpaper) result.modules[4].residencyKnown = false;
            continue;
        }
        bool taskbarImage = false, windowImage = false, wallpaperImage = false, entryImage = false;
        MODULEENTRY32W module{sizeof(module)};
        if (Module32FirstW(modules, &module)) do
        {
            entryImage |= _wcsicmp(module.szModule, L"SnowDesktopHookEntry.dll") == 0 ||
                _wcsicmp(module.szModule, L"SnowDesktopHookEntry32.dll") == 0;
            taskbarImage |= _wcsicmp(module.szModule, L"SnowDesktopTaskbarHook.dll") == 0;
            windowImage |= _wcsicmp(module.szModule, L"SnowDesktopDockMinimizeHook.dll") == 0 ||
                _wcsicmp(module.szModule, L"SnowDesktopDockMinimizeHook32.dll") == 0;
            wallpaperImage |= _wcsnicmp(module.szModule, L"SnowDesktopWallpaperHook", 24) == 0;
        } while (Module32NextW(modules, &module));
        CloseHandle(modules);
        const auto append = [&](std::size_t i) { result.modules[i].targets.push_back({process.th32ProcessID, process.szExeFile, explorer}); };
        if ((taskbarImage || entryImage) && explorer) { append(0); append(1); append(3); }
        if ((taskbarImage || entryImage) && start) append(3);
        // The stable entry can outlive its payload after controlled cleanup.
        // In application processes it is also part of the window module residency.
        if (windowImage || (entryImage && !explorer && !start)) append(2);
        if (wallpaperImage) append(4);
    } while (Process32NextW(processes, &process));
    CloseHandle(processes);
    return result;
}
}
