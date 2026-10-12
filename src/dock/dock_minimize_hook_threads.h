#pragma once

#include <windows.h>
#include <span>
#include <unordered_map>
#include <algorithm>
#include <string>

namespace snowdesktop::dock_minimize
{
class ThreadHooks final
{
public:
    ~ThreadHooks() { Stop(); }
    ThreadHooks() = default;
    ThreadHooks(const ThreadHooks&) = delete;
    ThreadHooks& operator=(const ThreadHooks&) = delete;

    bool Start(const std::wstring& path)
    {
        if (module_) return true;
        module_ = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) return false;
#ifdef _WIN64
        constexpr char exportName[] = "SnowDesktopDockMinimizeHook";
        constexpr char cloakExport[] = "SnowDesktopDockSourceCloakHook";
#else
        constexpr char exportName[] = "_SnowDesktopDockMinimizeHook@12";
        constexpr char cloakExport[] = "_SnowDesktopDockSourceCloakHook@12";
#endif
        procedure_ = reinterpret_cast<HOOKPROC>(GetProcAddress(module_, exportName));
        cloakProcedure_ = reinterpret_cast<HOOKPROC>(GetProcAddress(module_, cloakExport));
        if (!procedure_ || !cloakProcedure_) { Stop(); return false; }
        return true;
    }

    void Sync(std::span<const DWORD> threads)
    {
        for (auto it = hooks_.begin(); it != hooks_.end();)
        {
            if (std::find(threads.begin(), threads.end(), it->first) == threads.end())
            {
                UnhookWindowsHookEx(it->second.minimize);
                UnhookWindowsHookEx(it->second.cloak);
                it = hooks_.erase(it);
            }
            else ++it;
        }
        if (!procedure_) return;
        for (const DWORD thread : threads)
        {
            if (thread && !hooks_.contains(thread))
            {
                const HHOOK minimize = SetWindowsHookExW(WH_CBT, procedure_, module_, thread);
                const HHOOK cloak = minimize
                    ? SetWindowsHookExW(WH_CALLWNDPROC, cloakProcedure_, module_, thread) : nullptr;
                if (minimize && cloak) hooks_.emplace(thread, Hooks{minimize, cloak});
                else
                {
                    if (minimize) UnhookWindowsHookEx(minimize);
                    if (cloak) UnhookWindowsHookEx(cloak);
                }
            }
        }
    }

    bool HasThread(DWORD thread) const noexcept { return hooks_.contains(thread); }

    void Stop() noexcept
    {
        for (const auto& [thread, hook] : hooks_)
        {
            (void)thread;
            UnhookWindowsHookEx(hook.minimize);
            UnhookWindowsHookEx(hook.cloak);
        }
        hooks_.clear();
        procedure_ = nullptr;
        cloakProcedure_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
    }

private:
    HMODULE module_ = nullptr;
    HOOKPROC procedure_ = nullptr;
    HOOKPROC cloakProcedure_ = nullptr;
    struct Hooks { HHOOK minimize; HHOOK cloak; };
    std::unordered_map<DWORD, Hooks> hooks_;
};

inline bool Is32BitProcess(DWORD process) noexcept
{
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) return false;
    BOOL wow64 = FALSE;
    const bool result = IsWow64Process(handle, &wow64) && wow64;
    CloseHandle(handle);
    return result;
}
}
