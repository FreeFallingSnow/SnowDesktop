#pragma once
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace snowdesktop::steam_bridge
{
inline bool PrepareWorkshopStaging(const std::filesystem::path& root, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (ec || attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    { error = "Workshop staging directory is unavailable or unsafe"; return false; }
    // CLI and Manager share this directory. Each operation cleans only the
    // files it created; startup cannot infer ownership from directory age.
    return true;
}

struct WorkshopUploadDirectory
{
    std::filesystem::path root;
    std::vector<std::filesystem::path> files;
    ~WorkshopUploadDirectory()
    {
        if (root.empty()) return;
        const auto attributes = GetFileAttributesW(root.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
        std::error_code ec;
        for (const auto& file : files)
        {
            if (file.parent_path() == root) std::filesystem::remove(file, ec);
            ec.clear();
        }
        std::filesystem::remove(root, ec);
    }
};

// Share-denied handle ownership serializes read/compare/replace across both
// processes and threads. Only the live handle owns the lock; close/crash also
// removes the marker so legacy-root migration can still remove an empty root.
class WorkshopFileLock
{
public:
    explicit WorkshopFileLock(const std::filesystem::path& root)
    {
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) return;
        const auto path = root / L"projects.lock";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do
        {
            handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                BY_HANDLE_FILE_INFORMATION info{};
                if (!GetFileInformationByHandle(handle_, &info) ||
                    (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
                return;
            }
            if (GetLastError() != ERROR_SHARING_VIOLATION) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
    }
    ~WorkshopFileLock() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
    WorkshopFileLock(const WorkshopFileLock&) = delete;
    WorkshopFileLock& operator=(const WorkshopFileLock&) = delete;
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
}
