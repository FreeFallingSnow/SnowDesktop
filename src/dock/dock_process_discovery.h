#pragma once

#include <windows.h>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop::dock_process_discovery
{
// Retaining the process object binds the PID to this creation, even if it
// exits during enumeration. Never retain these handles beyond one pass.
class ProcessImage
{
public:
    ProcessImage() = default;
    ~ProcessImage() { if (handle_) CloseHandle(handle_); }
    ProcessImage(const ProcessImage&) = delete;
    ProcessImage& operator=(const ProcessImage&) = delete;
    ProcessImage(ProcessImage&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          canWait_(other.canWait_), identityKnown_(other.identityKnown_) {}
    ProcessImage& operator=(ProcessImage&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_) CloseHandle(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
            canWait_ = other.canWait_;
            identityKnown_ = other.identityKnown_;
        }
        return *this;
    }

    static ProcessImage Open(DWORD processId)
    {
        ProcessImage result;
        if (!processId) return result;
        result.handle_ = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
        result.canWait_ = result.handle_ != nullptr;
        // A process may grant image queries without granting synchronization.
        // Preserve that query, but do not cache an unverifiable lifetime.
        if (!result.handle_)
            result.handle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (result.handle_)
        {
            FILETIME creation{}, exitTime{}, kernel{}, user{};
            result.identityKnown_ = GetProcessTimes(
                result.handle_, &creation, &exitTime, &kernel, &user) != FALSE;
        }
        return result;
    }

    bool CanReuse() const
    {
        return handle_ && canWait_ && identityKnown_ &&
            WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
    }

    std::wstring ReadPath() const
    {
        if (!handle_) return {};
        wchar_t path[32768]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        return QueryFullProcessImageNameW(handle_, 0, path, &length)
            ? std::wstring(path, length) : std::wstring{};
    }

private:
    HANDLE handle_ = nullptr;
    bool canWait_ = false;
    bool identityKnown_ = false;
};

template<class Process = ProcessImage>
class ExecutablePaths
{
public:
    template<class Normalize>
    std::wstring Read(DWORD processId, Normalize normalize)
    {
        if (!processId) return {};
        const auto found = entries_.find(processId);
        if (found != entries_.end())
        {
            if (found->second.process.CanReuse()) return found->second.path;
            entries_.erase(found);
        }
        auto process = Process::Open(processId);
        std::wstring path = normalize(process.ReadPath());
        // Failed queries must remain retryable in this same enumeration.
        if (path.empty() || !process.CanReuse()) return path;
        const auto inserted = entries_.emplace(processId,
            Entry{std::move(process), std::move(path)});
        return inserted.first->second.path;
    }

private:
    struct Entry
    {
        Process process;
        std::wstring path;
    };
    std::unordered_map<DWORD, Entry> entries_;
};

using ParentMap = std::unordered_map<DWORD, DWORD>;

struct Snapshot
{
    // Parent edges and image metadata share one enumeration's lifetime. HWND
    // eligibility, AppID, ranking and model updates stay with their UI caller.
    ParentMap parents;
    mutable ExecutablePaths<> executablePaths;
};

template<class ReadPath>
std::vector<std::wstring> AncestorPaths(
    DWORD processId, const ParentMap& parents, ReadPath readPath)
{
    std::vector<std::wstring> paths;
    std::unordered_set<DWORD> visited{processId};
    for (size_t depth = 0; depth < 32; ++depth)
    {
        const auto found = parents.find(processId);
        if (found == parents.end() || !found->second ||
            !visited.insert(found->second).second)
            break;
        processId = found->second;
        std::wstring path = readPath(processId);
        if (path.empty()) break;
        paths.push_back(std::move(path));
    }
    return paths;
}
}
