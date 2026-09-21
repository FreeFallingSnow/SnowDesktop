#pragma once

#include "shell_change_notification.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::shell_refresh
{
inline std::wstring FolderKey(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    path = std::filesystem::path(path).lexically_normal().wstring();
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    CharUpperBuffW(path.data(), static_cast<DWORD>(path.size()));
    return path;
}

inline bool FolderContainsChange(const std::wstring& folder,
    const std::wstring& path, LONG event)
{
    if (path.empty()) return false;
    const auto key = FolderKey(path);
    if (key == folder) return true; // Includes coalesced SHCNE_UPDATEDIR.
    const auto prefix = folder.ends_with(L"\\") ? folder : folder + L"\\";
    if (key.starts_with(prefix) && key.find(L'\\', prefix.size()) == std::wstring::npos)
        return true;
    const auto ancestor = key.ends_with(L"\\") ? key : key + L"\\";
    return (event & (SHCNE_RENAMEFOLDER | SHCNE_RMDIR)) != 0 &&
        folder.starts_with(ancestor);
}

// Owned by the UI thread. One non-recursive registration per physical path,
// shared by all mapped widgets and the currently open ordinary Dock folder.
class FolderNotifications
{
public:
    struct FreePidl
    {
        using pointer = PIDLIST_ABSOLUTE;
        void operator()(pointer value) const { ILFree(value); }
    };
    struct ResolvedFolder
    {
        std::unique_ptr<ITEMIDLIST, FreePidl> pidl;
        std::wstring canonical;
    };
    using Resolver = std::function<ResolvedFolder(const std::wstring&)>;

    static ResolvedFolder Resolve(const std::wstring& path)
    {
        PIDLIST_ABSOLUTE raw = nullptr;
        const auto hr = SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr);
        ResolvedFolder result{std::unique_ptr<ITEMIDLIST, FreePidl>(raw), path};
        if (FAILED(hr)) { result.pidl.reset(); return result; }
        // Shell expands 8.3 names in events. Resolve aliases on the worker too.
        std::vector<wchar_t> resolved(32768);
        if (SHGetPathFromIDListEx(raw, resolved.data(),
            static_cast<DWORD>(resolved.size()), GPFIDL_DEFAULT))
            result.canonical = FolderKey(resolved.data());
        return result;
    }

    explicit FolderNotifications(Resolver resolver = Resolve)
        : worker_(std::make_shared<WorkerState>(std::move(resolver))) {}
    FolderNotifications(const FolderNotifications&) = delete;
    FolderNotifications& operator=(const FolderNotifications&) = delete;
    ~FolderNotifications()
    {
        Clear();
        {
            std::lock_guard lock(worker_->mutex);
            worker_->stopped = true;
        }
        worker_->changed.notify_all();
        // A Shell provider may never return. Workers own only their mailbox,
        // never this/UI objects; destruction must not join a blocked provider.
    }

    void Clear()
    {
        for (const auto& [path, id] : registrations_) SHChangeNotifyDeregister(id);
        registrations_.clear();
        aliases_.clear();
        requests_.clear();
        {
            std::lock_guard lock(worker_->mutex);
            worker_->live.clear();
            worker_->queue.clear();
            worker_->ready.clear();
            worker_->window = nullptr;
        }
        window_ = nullptr;
    }

    std::vector<std::wstring> Sync(HWND window, UINT message, UINT readyMessage,
        const std::vector<std::wstring>& paths)
    {
        if (syncing_) return {};
        syncing_ = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{syncing_};
        if (window_ != window || message_ != message) Clear();
        window_ = window;
        message_ = message;
        std::unordered_set<std::wstring> desired;
        for (const auto& path : paths)
            if (!path.empty()) desired.insert(FolderKey(path));
        std::erase_if(aliases_, [&](const auto& entry) { return !desired.contains(entry.first); });
        std::erase_if(requests_, [&](const auto& entry) { return !desired.contains(entry.first); });
        std::vector<std::wstring> added;
        if (!window || !IsWindow(window)) { Clear(); return added; }
        std::deque<Completion> ready;
        {
            std::lock_guard lock(worker_->mutex);
            ready.swap(worker_->ready);
        }
        for (auto& completion : ready)
        {
            const auto& path = completion.path;
            const auto request = requests_.find(path);
            if (request == requests_.end() || request->second.id != completion.id) continue;
            request->second.retryAfter = Clock::now() + std::chrono::seconds(30);
            if (!completion.folder.pidl) continue;
            const auto& canonical = completion.folder.canonical;
            if (registrations_.contains(canonical))
            {
                aliases_.emplace(path, canonical);
                added.push_back(path);
                requests_.erase(request);
                continue;
            }
            const SHChangeNotifyEntry entry{completion.folder.pidl.get(), FALSE};
            const ULONG id = SHChangeNotifyRegister(window,
                SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
                    SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
                    SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES,
                message, 1, &entry);
            if (id)
            {
                registrations_.emplace(canonical, id);
                aliases_.emplace(path, canonical);
                added.push_back(path);
                requests_.erase(request);
            }
        }
        if (!desired.empty()) StartWorkers();
        {
            std::lock_guard lock(worker_->mutex);
            worker_->window = window;
            worker_->message = readyMessage;
            worker_->live.clear();
            for (const auto& path : desired)
            {
                if (aliases_.contains(path)) continue;
                auto request = requests_.find(path);
                if (request == requests_.end() || Clock::now() >= request->second.retryAfter)
                {
                    const auto id = ++nextRequest_;
                    requests_[path] = {id, Clock::time_point::max()};
                    worker_->queue.push_back({path, id});
                }
                worker_->live.insert(requests_.at(path).id);
            }
            std::erase_if(worker_->queue, [&](const auto& job) { return !worker_->live.contains(job.id); });
        }
        worker_->changed.notify_all();
        std::unordered_set<std::wstring> retained;
        for (const auto& [path, canonical] : aliases_) retained.insert(canonical);
        std::erase_if(registrations_, [&](const auto& entry) {
            if (retained.contains(entry.first)) return false;
            SHChangeNotifyDeregister(entry.second);
            return true;
        });
        return added;
    }

    std::vector<std::wstring> Affected(
        const std::optional<ShellChangeNotification>& change) const
    {
        std::vector<std::wstring> paths;
        for (const auto& [path, canonical] : aliases_)
            if (!change || (change->source.empty() && change->target.empty()) ||
                FolderContainsChange(path, change->source, change->event) ||
                FolderContainsChange(path, change->target, change->event) ||
                FolderContainsChange(canonical, change->source, change->event) ||
                FolderContainsChange(canonical, change->target, change->event))
                paths.push_back(path);
        return paths;
    }

    size_t Size() const { return registrations_.size(); }

private:
    using Clock = std::chrono::steady_clock;
    struct Job { std::wstring path; unsigned long long id; };
    struct Completion : Job { ResolvedFolder folder; };
    struct RequestState { unsigned long long id; Clock::time_point retryAfter; };
    struct WorkerState
    {
        explicit WorkerState(Resolver value) : resolve(std::move(value)) {}
        Resolver resolve;
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Job> queue;
        std::deque<Completion> ready;
        std::unordered_set<unsigned long long> live;
        HWND window = nullptr;
        UINT message = 0;
        bool stopped = false;
    };

    void StartWorkers()
    {
        // Reuse the same two workers through Clear/window recovery. Rebuilding
        // or reopening a stalled path must not create unbounded blocked threads.
        while (workerCount_ < 2)
        {
            std::thread([state = worker_] {
                const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                for (;;)
                {
                    Job job;
                    {
                        std::unique_lock lock(state->mutex);
                        state->changed.wait(lock, [&] { return state->stopped || !state->queue.empty(); });
                        if (state->stopped) break;
                        job = std::move(state->queue.front());
                        state->queue.pop_front();
                    }
                    ResolvedFolder folder;
                    if (SUCCEEDED(initialized))
                    {
                        try { folder = state->resolve(job.path); }
                        catch (...) { /* Treat provider failure like an unavailable path. */ }
                    }
                    std::lock_guard lock(state->mutex);
                    if (state->stopped || !state->live.contains(job.id)) continue;
                    state->ready.push_back({std::move(job), std::move(folder)});
                    // No pointers in the message. Sync validates every request
                    // against the current paths even if a wake was already queued.
                    if (state->window) PostMessageW(state->window, state->message, 0, 0);
                }
                if (SUCCEEDED(initialized)) CoUninitialize();
            }).detach();
            ++workerCount_;
        }
    }

    std::shared_ptr<WorkerState> worker_;
    unsigned workerCount_ = 0;
    unsigned long long nextRequest_ = 0;
    std::unordered_map<std::wstring, RequestState> requests_;
    HWND window_ = nullptr;
    UINT message_ = 0;
    bool syncing_ = false;
    std::unordered_map<std::wstring, ULONG> registrations_;
    std::unordered_map<std::wstring, std::wstring> aliases_;
};

// Keep the whole pending scope until an up-to-date read is applied. An event
// arriving during a read must not lose earlier directories when that read is
// rejected by Revision. An empty scope represents a full Shell refresh.
class FolderRefreshScope
{
public:
    void Full() { folders_.clear(); }
    void Add(const std::vector<std::wstring>& paths, bool alreadyPending)
    {
        if (alreadyPending && folders_.empty()) return;
        if (!alreadyPending) folders_.clear();
        for (const auto& path : paths) folders_.insert(FolderKey(path));
    }
    bool FoldersOnly() const { return !folders_.empty(); }
    bool Includes(const std::wstring& path) const
    {
        return folders_.empty() || folders_.contains(FolderKey(path));
    }
private:
    std::unordered_set<std::wstring> folders_;
};
}
