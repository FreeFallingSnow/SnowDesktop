// SPDX-License-Identifier: MIT
#pragma once
#include "theme_workshop_publish.h"
namespace snowdesktop::steam_bridge
{
inline constexpr unsigned kIconPackWorkflowProtocolVersion = 1;
struct IconPackPublishPlan
{
    std::filesystem::path directory, package, preview, association;
    std::string packageId, title, description, packageSha256, coverSha256, snapshotHash, preparationSha256;
    std::uint64_t publishedFileId = 0;
    std::int64_t preparedAt = 0;
};
std::string IconPackFileHash(const std::filesystem::path&);
bool BuildIconPackPublishPlan(const std::filesystem::path&, const std::filesystem::path&, IconPackPublishPlan&, std::string&);
std::string IconPackPublishPlanJson(const IconPackPublishPlan&);
std::string IconPackPublishedUrl(const std::filesystem::path& data, std::string_view groupId);
bool ExecuteIconPackPublishPlan(const IconPackPublishPlan&, bool confirmCreate, bool confirmUpdate,
    ThemePublishTransport&, const PublishProgressCallback&, PublishResult&, CoreError&, std::int64_t now,
    const std::atomic_bool* cancel = nullptr);
}
