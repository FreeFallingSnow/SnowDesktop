#pragma once

#include <windows.h>
#include <span>
#include <unordered_map>
#include <algorithm>
#include <string>
#include "dock_cloak_dispatcher.h"

namespace snowdesktop::dock_minimize
{
class ThreadHooks final
{
public:
    ~ThreadHooks() { Stop(); }
    ThreadHooks(const ThreadHooks&) = delete;
    ThreadHooks& operator=(const ThreadHooks&) = delete;

    explicit ThreadHooks(HookApi api = {}) : api_(api) {}
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
        if (!procedure_ || !cloakProcedure_ || !dispatcher_.Start(module_, cloakProcedure_, api_))
        { Stop(); return false; }
        return true;
    }

    void Sync(std::span<const DWORD> threads)
    {
        for (auto it = hooks_.begin(); it != hooks_.end();)
        {
            if (std::find(threads.begin(), threads.end(), it->first) == threads.end())
            {
                api_.remove(it->second);
                it = hooks_.erase(it);
            }
            else ++it;
        }
        for (auto it = retries_.begin(); it != retries_.end();)
            if (std::find(threads.begin(), threads.end(), it->first) == threads.end()) it = retries_.erase(it);
            else ++it;
        if (!procedure_) return;
        for (const DWORD thread : threads)
        {
            if (thread && !hooks_.contains(thread))
            {
                auto& retry = retries_[thread];
                if (!retry.Ready(GetTickCount64())) continue;
                SetLastError(0);
                const HHOOK minimize = api_.install(WH_CBT, procedure_, module_, thread);
                ++statistics_.attempts; statistics_.lastThread = thread;
                if (minimize)
                { hooks_.emplace(thread, minimize); retries_.erase(thread); ++statistics_.successes; statistics_.lastError = 0; }
                else
                {
                    const DWORD error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
                    ++statistics_.failures; statistics_.lastError = error;
                    retry.Failed(error, GetTickCount64());
                }
            }
        }
    }

    bool HasThread(DWORD thread) const noexcept { return hooks_.contains(thread); }
    bool HasPendingRetries() const noexcept { return !retries_.empty(); }
    std::size_t HookCount() const noexcept { return hooks_.size(); }
    HWND CloakWindow() const noexcept { return dispatcher_.Window(); }
    hook_registration::Statistics RegistrationStatistics() const noexcept { return statistics_; }
    hook_registration::Statistics CloakStatistics() const { return dispatcher_.Statistics(); }

    void Stop() noexcept
    {
        for (const auto& [thread, hook] : hooks_)
        {
            (void)thread;
            api_.remove(hook);
        }
        hooks_.clear();
        retries_.clear();
        dispatcher_.Stop();
        procedure_ = nullptr;
        cloakProcedure_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
    }

private:
    HMODULE module_ = nullptr;
    HOOKPROC procedure_ = nullptr;
    HOOKPROC cloakProcedure_ = nullptr;
    HookApi api_;
    CloakDispatcher dispatcher_;
    hook_registration::Statistics statistics_;
    std::unordered_map<DWORD, HHOOK> hooks_;
    std::unordered_map<DWORD, hook_registration::Retry> retries_;
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
