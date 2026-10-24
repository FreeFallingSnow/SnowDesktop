#pragma once

#include <windows.h>
#include <unordered_map>

namespace snowdesktop::dock_minimize
{
struct ProcessBitnessApi
{
    decltype(&OpenProcess) open = OpenProcess;
    decltype(&IsWow64Process) query = IsWow64Process;
    decltype(&WaitForSingleObject) wait = WaitForSingleObject;
    decltype(&CloseHandle) close = CloseHandle;
};

// A retained, unsignaled process handle identifies a lifetime, even when a PID
// is recycled. Failed queries are never cached, so access changes can recover.
class ProcessBitnessCache final
{
public:
    explicit ProcessBitnessCache(ProcessBitnessApi api = {}) : api_(api) {}
    ~ProcessBitnessCache() { Clear(); }
    ProcessBitnessCache(const ProcessBitnessCache&) = delete;
    ProcessBitnessCache& operator=(const ProcessBitnessCache&) = delete;

    void BeginUpdate() noexcept
    {
        for (auto& [process, entry] : entries_)
        {
            (void)process;
            entry.used = false;
        }
    }

    bool Is32Bit(DWORD process)
    {
        if (auto found = entries_.find(process); found != entries_.end())
        {
            if (api_.wait(found->second.handle, 0) == WAIT_TIMEOUT)
            {
                found->second.used = true;
                return found->second.wow64;
            }
            api_.close(found->second.handle);
            entries_.erase(found);
        }
        HANDLE handle = api_.open(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process);
        const bool canRetain = handle != nullptr;
        // Preserve the original query-only access fallback for restricted apps.
        if (!handle) handle = api_.open(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
        if (!handle) return false;
        BOOL wow64 = FALSE;
        const bool known = api_.query(handle, &wow64) != FALSE;
        if (known && canRetain && api_.wait(handle, 0) == WAIT_TIMEOUT)
            entries_.emplace(process, Entry{handle, wow64 != FALSE, true});
        else api_.close(handle);
        return known && wow64;
    }

    void EndUpdate() noexcept
    {
        for (auto it = entries_.begin(); it != entries_.end();)
            if (!it->second.used)
            {
                api_.close(it->second.handle);
                it = entries_.erase(it);
            }
            else ++it;
    }

    void Clear() noexcept
    {
        for (const auto& [process, entry] : entries_)
        {
            (void)process;
            api_.close(entry.handle);
        }
        entries_.clear();
    }

private:
    struct Entry
    {
        HANDLE handle = nullptr;
        bool wow64 = false;
        bool used = false;
    };
    ProcessBitnessApi api_;
    std::unordered_map<DWORD, Entry> entries_;
};
}
