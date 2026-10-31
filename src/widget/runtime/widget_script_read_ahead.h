#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
// Notifications are read-ahead hints, never proof that a script is unchanged.
// RenderWidget keeps its synchronous metadata check and ReloadWidget fallback.
// No source bytes, failure state, or callbacks are delivered back to the VM.
class WidgetScriptReadAhead final
{
public:
    struct Snapshot
    {
        std::uint64_t appliedRevision = 0;
        std::uint64_t scans = 0;
        std::uint64_t failedPreparations = 0;
        std::size_t files = 0;
        std::size_t watches = 0;
        bool running = false;
    };

    WidgetScriptReadAhead() : state_(std::make_shared<State>()) {}
    ~WidgetScriptReadAhead() { Stop(); }
    WidgetScriptReadAhead(const WidgetScriptReadAhead&) = delete;
    WidgetScriptReadAhead& operator=(const WidgetScriptReadAhead&) = delete;

    bool Track(const std::wstring& instance, const std::wstring& script,
        const std::wstring& manifest, const std::wstring& installedRoot = {}) noexcept
    {
        try
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopped || !state_->wake || instance.empty() ||
                !std::filesystem::path(script).is_absolute() ||
                !std::filesystem::path(manifest).is_absolute() ||
                (!installedRoot.empty() && !std::filesystem::path(installedRoot).is_absolute())) return false;
            const Tracking files{{script, manifest}, installedRoot};
            const auto found = state_->instances.find(instance);
            if (found != state_->instances.end() && found->second == files &&
                state_->snapshot.running)
                return true;
            // This is optional prefetch. Capacity exhaustion retains the normal
            // synchronous path rather than growing an unbounded watch service.
            if (found == state_->instances.end() &&
                state_->instances.size() >= MaximumInstances) return false;
            state_->instances.insert_or_assign(instance, files);
            ++state_->revision;
            if (!state_->snapshot.running)
            {
                state_->snapshot.running = true;
                try { std::thread([state = state_] { Run(state); }).detach(); }
                catch (...) { state_->snapshot.running = false; return false; }
            }
            SetEvent(state_->wake);
            return true;
        }
        catch (...) { return false; }
    }

    void Forget(const std::wstring& instance) noexcept
    {
        try
        {
            std::lock_guard lock(state_->mutex);
            if (state_->instances.erase(instance))
            {
                ++state_->revision;
                if (state_->wake) SetEvent(state_->wake);
            }
        }
        catch (...) { /* Optional read-ahead must not affect unloading. */ }
    }

    void Stop() noexcept
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        state_->instances.clear();
        if (state_->wake) SetEvent(state_->wake);
    }

    Snapshot Inspect() const
    {
        std::lock_guard lock(state_->mutex);
        return state_->snapshot;
    }

    std::uint64_t PreparedCount(const std::wstring& path) const
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->prepared.find(path);
        return found == state_->prepared.end() ? 0 : found->second;
    }

private:
    static constexpr std::size_t MaximumInstances = 256;
    static constexpr std::size_t MaximumWatches = MAXIMUM_WAIT_OBJECTS - 1;
    static constexpr std::size_t MaximumReadBytes = 1024 * 1024;
    using Files = std::array<std::wstring, 2>;
    struct Tracking
    {
        Files files;
        std::wstring installedRoot;
        bool operator==(const Tracking&) const = default;
    };
    using TrackedFiles = std::map<std::wstring, std::wstring>;
    struct State
    {
        std::mutex mutex;
        HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::map<std::wstring, Tracking> instances;
        std::map<std::wstring, std::uint64_t> prepared;
        std::uint64_t revision = 0;
        Snapshot snapshot;
        bool stopped = false;
        ~State() { if (wake) CloseHandle(wake); }
    };
    struct Watch
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~Watch()
        {
            if (handle != INVALID_HANDLE_VALUE)
                FindCloseChangeNotification(handle);
        }
    };
    struct Stamp
    {
        DWORD createdLow, createdHigh, modifiedLow, modifiedHigh, sizeLow, sizeHigh;
        bool operator==(const Stamp&) const = default;
    };

    static std::optional<Stamp> ReadStamp(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
        return Stamp{data.ftCreationTime.dwLowDateTime, data.ftCreationTime.dwHighDateTime,
            data.ftLastWriteTime.dwLowDateTime, data.ftLastWriteTime.dwHighDateTime,
            data.nFileSizeLow, data.nFileSizeHigh};
    }

    static bool IsLocalDirectory(const std::filesystem::path& directory)
    {
        std::array<wchar_t, 32768> volume{};
        if (!GetVolumePathNameW(directory.c_str(), volume.data(),
                static_cast<DWORD>(volume.size())) ||
            GetDriveTypeW(volume.data()) != DRIVE_FIXED) return false;
        for (auto path = directory; !path.empty();)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            const auto parent = path.parent_path();
            if (parent == path) break;
            path = parent;
        }
        return true;
    }

    static bool Warm(const std::wstring& path, const std::shared_ptr<State>& state)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const std::unique_ptr<void, decltype(&CloseHandle)> ownedFile(file, CloseHandle);
        std::array<char, 64 * 1024> bytes{};
        bool success = true;
        for (std::size_t total = 0; total < MaximumReadBytes;)
        {
            {
                std::lock_guard lock(state->mutex);
                if (state->stopped) { success = false; break; }
            }
            DWORD read = 0;
            if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
            { success = false; break; }
            if (!read) break;
            total += read;
        }
        return success;
    }

    static std::wstring StableWatchRoot(const std::wstring& file,
        const std::wstring& installedRoot)
    {
        if (installedRoot.empty()) return {};
        auto root = std::filesystem::path(installedRoot).lexically_normal();
        if (root.filename().empty() && root != root.root_path()) root = root.parent_path();
        const auto source = std::filesystem::path(file).lexically_normal();
        auto part = source.begin();
        for (const auto& component : root)
        {
            if (part == source.end() || _wcsicmp(part->c_str(), component.c_str()) != 0)
                return {};
            ++part;
        }
        return part == source.end() ? std::wstring{} : root.wstring();
    }

    static std::vector<std::unique_ptr<Watch>> OpenWatches(
        const TrackedFiles& files)
    {
        constexpr DWORD sourceChanges = FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES |
            FILE_NOTIFY_CHANGE_SECURITY;
        constexpr DWORD parentChanges = FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY;
        struct Request { DWORD changes = 0; bool subtree = false; };
        std::map<std::wstring, Request> directories;
        for (const auto& [file, stableRoot] : files)
        {
            // Uninstall moves installed/<UUID>, above the watched version.
            // A child directory notification handle prevents that parent move
            // on Windows, even though renaming the watched directory itself is
            // allowed. Keep installed notifications outside every package tree.
            const auto parent = stableRoot.empty()
                ? std::filesystem::path(file).parent_path()
                : std::filesystem::path(stableRoot);
            auto& request = directories[parent.wstring()];
            request.changes |= sourceChanges;
            request.subtree = request.subtree || !stableRoot.empty();
            // A directory watch does not report replacement of its own root.
            directories[parent.parent_path().wstring()].changes |= parentChanges;
        }
        std::vector<std::unique_ptr<Watch>> watches;
        for (const auto& [directory, request] : directories)
        {
            if (watches.size() == MaximumWatches) break;
            if (!IsLocalDirectory(directory)) continue;
            auto watch = std::make_unique<Watch>();
            watch->handle = FindFirstChangeNotificationW(directory.c_str(), request.subtree, request.changes);
            if (watch->handle != INVALID_HANDLE_VALUE)
                watches.push_back(std::move(watch));
        }
        return watches;
    }

    static void Run(const std::shared_ptr<State>& state) noexcept
    {
        // Advisory I/O should yield to the foreground loader and renderer.
        (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        try
        {
            TrackedFiles files;
            std::set<std::wstring> knownFiles;
            std::map<std::wstring, Stamp> stamps;
            std::vector<std::unique_ptr<Watch>> watches;
            bool rebuild = true;
            std::uint64_t revision = 0;
            for (;;)
            {
                std::optional<TrackedFiles> requestedFiles;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->stopped) break;
                    if (revision != state->revision)
                    {
                        requestedFiles.emplace();
                        for (const auto& [id, tracking] : state->instances)
                        {
                            (void)id;
                            for (const auto& path : tracking.files)
                                requestedFiles->try_emplace(path,
                                    StableWatchRoot(path, tracking.installedRoot));
                        }
                        revision = state->revision;
                        rebuild = true;
                    }
                    ResetEvent(state->wake);
                }
                // Resolve volume eligibility outside the mailbox lock: a slow
                // filesystem must never make Track/Forget/Stop wait for I/O.
                if (requestedFiles)
                {
                    files.clear();
                    for (const auto& [path, stableRoot] : *requestedFiles)
                        if (IsLocalDirectory(std::filesystem::path(path).parent_path()))
                            files.emplace(path, stableRoot);
                }
                if (rebuild)
                {
                    watches = OpenWatches(files);
                    std::erase_if(stamps, [&](const auto& item) { return !files.contains(item.first); });
                    std::erase_if(knownFiles, [&](const auto& file) { return !files.contains(file); });
                    for (const auto& [file, stableRoot] : files)
                    {
                        (void)stableRoot;
                        // Loading already read a newly tracked source. Establish
                        // its baseline after arming, without rereading that revision.
                        if (knownFiles.insert(file).second)
                            if (const auto stamp = ReadStamp(file)) stamps.emplace(file, *stamp);
                    }
                    rebuild = false;
                    std::lock_guard lock(state->mutex);
                    state->snapshot.appliedRevision = revision;
                    state->snapshot.files = files.size();
                    state->snapshot.watches = watches.size();
                    std::erase_if(state->prepared, [&](const auto& item) { return !files.contains(item.first); });
                }
                // New watches are armed before rereading; an edit that occurs
                // during preparation signals the next pass. Reopen after a hint
                // because an editor can replace an entire package directory.
                for (const auto& [file, stableRoot] : files)
                {
                    (void)stableRoot;
                    if (!IsLocalDirectory(std::filesystem::path(file).parent_path()))
                    {
                        stamps.erase(file);
                        continue;
                    }
                    const auto stamp = ReadStamp(file);
                    if (!stamp)
                    {
                        stamps.erase(file);
                        std::lock_guard lock(state->mutex);
                        ++state->snapshot.failedPreparations;
                        continue;
                    }
                    const auto previous = stamps.find(file);
                    if (previous != stamps.end() && previous->second == *stamp) continue;
                    if (Warm(file, state) && ReadStamp(file) == stamp)
                    {
                        stamps.insert_or_assign(file, *stamp);
                        std::lock_guard lock(state->mutex);
                        if (!state->stopped) ++state->prepared[file];
                    }
                    else
                    {
                        stamps.erase(file);
                        std::lock_guard lock(state->mutex);
                        ++state->snapshot.failedPreparations;
                    }
                }
                {
                    std::lock_guard lock(state->mutex);
                    ++state->snapshot.scans;
                }
                std::vector<HANDLE> handles{state->wake};
                for (const auto& watch : watches) handles.push_back(watch->handle);
                const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                    handles.data(), FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) continue;
                if (result == WAIT_FAILED) break;
                rebuild = true;
            }
        }
        catch (...) { /* Failure only removes the optional pre-read hint. */ }
        std::lock_guard lock(state->mutex);
        state->snapshot.running = false;
        state->snapshot.files = state->snapshot.watches = 0;
    }

    // A blocked filesystem cannot retain the engine or hold up UI shutdown.
    // The detached worker owns only this state and releases its handles on exit.
    std::shared_ptr<State> state_;
};
}
