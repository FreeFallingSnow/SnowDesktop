#pragma once
#include "steam_workshop_core.h"
#include <atomic>

namespace snowdesktop::steam_bridge
{
inline constexpr unsigned kThemeWorkflowProtocolVersion = 1;
struct ThemeGalleryImage
{
    std::string component, sha256;
    std::filesystem::path path;
};
struct ThemePublishPlan
{
    std::filesystem::path directory, package, preview, association;
    std::string rootId, title, packageSha256, coverSha256, tagsSha256, gallerySha256;
    std::vector<std::string> tags;
    std::vector<ThemeGalleryImage> gallery;
    std::uint64_t publishedFileId = 0;
    std::int64_t preparedAt = 0;
};
std::string ThemeSha256(std::string_view bytes);
std::string ThemeFileSha256(const std::filesystem::path&);
bool ThemeSafePath(const std::filesystem::path&, bool directory = false);
bool WriteThemePreparation(const std::filesystem::path& directory, std::string_view rootId,
    std::string_view title, std::int64_t now, std::string& error, const std::vector<std::string>& tags);
std::string ThemePublishedUrl(const std::filesystem::path& dataDirectory, std::string_view rootId);
bool BuildThemePublishPlan(const std::filesystem::path& directory, const std::filesystem::path& dataDirectory,
    ThemePublishPlan&, std::string& error);
std::string ThemePublishPlanJson(const ThemePublishPlan&);
struct ThemePublishTransport
{
    std::function<std::optional<SteamStatus>(CoreError&)> status;
    std::function<std::optional<WorkshopEulaStatus>(CoreError&)> agreement;
    std::function<std::optional<PublishedItem>(std::uint64_t, CoreError&)> item;
    std::function<std::optional<PublishResult>(const PublishRequest&, const PublishProgressCallback&, CoreError&)> publish;
};
// All scheduling, locking, confirmations and created-ID persistence are real.
// Only the four Steam boundary calls are replaceable in offline tests.
bool ExecuteThemePublishPlan(const ThemePublishPlan&, bool confirmCreate, bool confirmUpdate,
    ThemePublishTransport&, const PublishProgressCallback&, PublishResult&, CoreError&,
    std::int64_t now, const std::atomic_bool* cancel = nullptr);
}
