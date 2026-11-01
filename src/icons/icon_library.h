#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include "icon_package.h"

namespace snowdesktop
{
struct IconLibraryGroup
{
    std::string id;
    std::wstring name;
    std::string source = "local", packageId, packageVersion, sourceUri;
    std::wstring shareTitle, description, license, author;
    bool rightsConfirmed = false;
    std::string workshopItem, workshopOwner, workshopHash, publishedUrl;
    std::vector<std::string> accounts;
};
struct IconLibraryEntry
{
    std::string id, format;
    std::wstring name, previewPath;
    int width = 0, height = 0;
    bool missing = false;
    std::string source = "local", packageId, packageVersion, packageEntryId;
    std::vector<std::wstring> uses;
    std::vector<std::string> groupIds;
    bool localOwned = true;
    std::map<std::string, std::wstring> groupNames;
};
struct IconLibraryRequest
{
    std::string action = "read"; // read, import, rename, delete, export, apply, cancel
    std::vector<std::string> ids;
    std::vector<std::wstring> paths;
    std::wstring name, destination, target;
    std::string format = "png", groupId;
    std::uint64_t session = 0, revision = 0;
    std::wstring title, description, license, coverPath;
    bool rightsConfirmed = false;
    std::string preparationId;
    std::uint64_t operationId = 0;
};
struct IconLibrarySnapshot
{
    std::vector<IconLibraryEntry> entries;
    std::vector<IconLibraryGroup> groups;
    std::vector<std::string> imported;
    std::string importedGroupId;
    std::vector<std::wstring> failures;
    std::uint64_t revision = 0, session = 0;
    bool busy = false, succeeded = false;
    std::string error;
    std::wstring target, targetName;
    bool workshopAvailable = false;
    std::wstring coverPath;
    std::string preparationId, publishedUrl, completedAction;
    std::uint64_t operationId = 0;
    unsigned progress = 0; // 0 idle, 1 preparing package, 2 rendering cover, 3 publishing, 4 synchronizing
};
struct IconPackDownload
{
    std::string item, owner, author, sha256;
    icon_package::Package package;
};
struct IconPackSubscriptions
{
    bool authoritative = false;
    std::string account;
    std::set<std::string> subscribed, deleted;
    std::vector<IconPackDownload> downloads;
    // Steam local caches have no verified online author identity.
    bool localCache = false;
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
struct LargeIconConfig;
// Reverse operations remain available without premium entitlement. Only the
// deleted library source is reset; sizing, effects and other sources survive.
bool RestoreIconLibraryAssetReferences(LargeIconConfig& config, const std::vector<std::string>& ids);

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
    bool ExportGroup(std::string_view id, icon_package::Package&, std::string& error);
    IconLibrarySnapshot Reconcile(const IconPackSubscriptions&);
    IconLibrarySnapshot CollectRetired(const std::vector<std::string>& protectedIds);
private:
    bool Load();
    bool Save();
    std::string Import(const std::filesystem::path& path, std::string& error,
        const std::string& groupId = {}, std::wstring name = {});
    std::filesystem::path directory_;
    std::vector<IconLibraryEntry> entries_;
    std::vector<IconLibraryGroup> groups_;
    std::map<std::string, std::set<std::string>> subscriptionAccounts_;
    bool loaded_ = false, valid_ = true;
    std::uint64_t revision_ = 0;
};
}
