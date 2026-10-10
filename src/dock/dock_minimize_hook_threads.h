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
#else
        constexpr char exportName[] = "_SnowDesktopDockMinimizeHook@12";
#endif
        procedure_ = reinterpret_cast<HOOKPROC>(GetProcAddress(module_, exportName));
        if (!procedure_) { Stop(); return false; }
        return true;
    }

    void Sync(std::span<const DWORD> threads)
    {
        for (auto it = hooks_.begin(); it != hooks_.end();)
        {
            if (std::find(threads.begin(), threads.end(), it->first) == threads.end())
            {
                UnhookWindowsHookEx(it->second);
                it = hooks_.erase(it);
            }
            else ++it;
        }
        if (!procedure_) return;
        for (const DWORD thread : threads)
        {
            if (thread && !hooks_.contains(thread))
            {
                if (const HHOOK hook = SetWindowsHookExW(WH_CBT, procedure_, module_, thread))
                    hooks_.emplace(thread, hook);
            }
        }
    }

    bool HasThread(DWORD thread) const noexcept { return hooks_.contains(thread); }

    void Stop() noexcept
    {
        for (const auto& [thread, hook] : hooks_)
        {
            (void)thread;
            UnhookWindowsHookEx(hook);
        }
        hooks_.clear();
        procedure_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
    }

private:
    HMODULE module_ = nullptr;
    HOOKPROC procedure_ = nullptr;
    std::unordered_map<DWORD, HHOOK> hooks_;
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
