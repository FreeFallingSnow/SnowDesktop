#pragma once

#include "shell_change_notification.h"
#include <algorithm>
#include <filesystem>
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
    FolderNotifications() = default;
    FolderNotifications(const FolderNotifications&) = delete;
    FolderNotifications& operator=(const FolderNotifications&) = delete;
    ~FolderNotifications() { Clear(); }

    void Clear()
    {
        for (const auto& [path, id] : registrations_) SHChangeNotifyDeregister(id);
        registrations_.clear();
        aliases_.clear();
        window_ = nullptr;
    }

    std::vector<std::wstring> Sync(HWND window, UINT message,
        const std::vector<std::wstring>& paths)
    {
        if (syncing_) return {};
        syncing_ = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{syncing_};
        if (window_ != window) Clear();
        window_ = window;
        std::unordered_set<std::wstring> desired;
        for (const auto& path : paths)
            if (!path.empty()) desired.insert(FolderKey(path));
        std::erase_if(aliases_, [&](const auto& entry) { return !desired.contains(entry.first); });
        std::vector<std::wstring> added;
        if (!window || !IsWindow(window)) { Clear(); return added; }
        for (const auto& path : desired)
        {
            if (aliases_.contains(path)) continue;
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)))
                continue; // Retry at the next model/popup synchronization.
            // Resolve once at registration, never do filesystem path queries
            // for every incoming event. Shell expands 8.3 names in its events.
            std::vector<wchar_t> resolved(32768);
            const auto canonical = SHGetPathFromIDListEx(pidl, resolved.data(),
                static_cast<DWORD>(resolved.size()), GPFIDL_DEFAULT)
                ? FolderKey(resolved.data()) : path;
            if (registrations_.contains(canonical))
            {
                aliases_.emplace(path, canonical);
                added.push_back(path);
                ILFree(pidl);
                continue;
            }
            const SHChangeNotifyEntry entry{pidl, FALSE};
            const ULONG id = SHChangeNotifyRegister(window,
                SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
                    SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
                    SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES,
                message, 1, &entry);
            ILFree(pidl);
            if (id)
            {
                registrations_.emplace(canonical, id);
                aliases_.emplace(path, canonical);
                added.push_back(path);
            }
        }
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
    HWND window_ = nullptr;
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
