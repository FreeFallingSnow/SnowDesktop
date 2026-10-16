#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{
struct IconLibraryEntry
{
    std::string id, format;
    std::wstring name, previewPath;
    int width = 0, height = 0;
    bool missing = false;
    std::string source = "local", packageId, packageVersion, packageEntryId;
    std::vector<std::wstring> uses;
};
struct IconLibraryRequest
{
    std::string action = "read"; // read, import, rename, delete, export, apply, cancel
    std::vector<std::string> ids;
    std::vector<std::wstring> paths;
    std::wstring name, destination, target;
    std::string format = "png";
    std::uint64_t session = 0, revision = 0;
};
struct IconLibrarySnapshot
{
    std::vector<IconLibraryEntry> entries;
    std::vector<std::string> imported;
    std::vector<std::wstring> failures;
    std::uint64_t revision = 0, session = 0;
    bool busy = false, succeeded = false;
    std::string error;
    std::wstring target, targetName;
};
using IconLibraryAction = std::function<IconLibrarySnapshot(IconLibraryRequest)>;

inline bool IsIconAssetId(std::string_view id)
{
    if (id.size() < 68 || id.size() > 70 || id[64] != '.') return false;
    for (size_t i = 0; i < 64; ++i) if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    const auto extension = id.substr(65);
    return extension == "png" || extension == "ico" || extension == "jpg" || extension == "bmp" || extension == "svg" || extension == "webp";
}
std::filesystem::path IconAssetPath(const std::filesystem::path& directory, std::string_view id);

// Synchronous storage operations run exclusively on the host's resource worker.
// UI and render code receive immutable snapshots/paths, never this mutable store.
class IconLibrary
{
public:
    explicit IconLibrary(std::filesystem::path directory);
    IconLibrarySnapshot Read();
    IconLibrarySnapshot Refresh();
    IconLibrarySnapshot Invoke(const IconLibraryRequest& request,
        const std::vector<std::string>& protectedIds = {});
private:
    bool Load();
    bool Save();
    std::string Import(const std::filesystem::path& path, std::string& error);
    std::filesystem::path directory_;
    std::vector<IconLibraryEntry> entries_;
    bool loaded_ = false, valid_ = true;
    std::uint64_t revision_ = 0;
};
}
