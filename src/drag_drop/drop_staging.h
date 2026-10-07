#pragma once

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::drop_staging
{
// Non-file OLE payloads are scratch files. Neither layouts nor shortcuts may
// retain these paths; reference destinations must copy to their final location.
inline bool RemoveTree(const std::filesystem::path& root)
{
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    std::error_code error;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return std::filesystem::remove(root, error) && !error;
    std::vector<std::filesystem::path> children;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
        children.push_back(it->path());
    if (error) return false;
    bool complete = true;
    for (const auto& child : children) if (!RemoveTree(child)) complete = false;
    return complete && std::filesystem::remove(root, error) && !error;
}

inline void CollectRetired(const std::filesystem::path& root)
{
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        const auto candidate = it->path();
        if (!candidate.filename().wstring().starts_with(L"session-") ||
            !it->is_directory(error) || error) continue;
        const auto marker = candidate / L".owner.lock";
        const DWORD markerAttributes = GetFileAttributesW(marker.c_str());
        if (markerAttributes == INVALID_FILE_ATTRIBUTES ||
            (markerAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        HANDLE probe = CreateFileW(marker.c_str(), DELETE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue; // A live process owns it.
        CloseHandle(probe);
        RemoveTree(candidate);
    }
}

class Session final
{
public:
    explicit Session(const std::filesystem::path& root)
    {
        if (root.empty() || !root.is_absolute()) return;
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error || (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) return;
        CollectRetired(root);
        static std::atomic<unsigned long long> serial = 0;
        path_ = root / (L"session-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial));
        if (!std::filesystem::create_directory(path_, error) || error) { path_.clear(); return; }
        owner_ = CreateFileW((path_ / L".owner.lock").c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (owner_ == INVALID_HANDLE_VALUE) { RemoveTree(path_); path_.clear(); }
    }
    ~Session()
    {
        if (owner_ != INVALID_HANDLE_VALUE) CloseHandle(owner_);
        if (!path_.empty()) RemoveTree(path_);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    const std::filesystem::path& Path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
    HANDLE owner_ = INVALID_HANDLE_VALUE;
};

inline std::filesystem::path Directory()
{
    static const Session session([] {
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        return error ? std::filesystem::path{} : temporary / L"SnowDesktop.DropContent";
    }());
    return session.Path();
}
inline bool Contains(const std::wstring& file)
{
    const auto directory = Directory();
    return !directory.empty() && _wcsicmp(std::filesystem::path(file).parent_path().c_str(), directory.c_str()) == 0;
}
}
