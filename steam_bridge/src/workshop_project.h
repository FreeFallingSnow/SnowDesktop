// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::steam_bridge
{
inline constexpr int kProjectStoreSchemaVersion = 1;

struct WorkshopProject
{
    std::string localId;
    std::filesystem::path sourceDirectory;
    std::filesystem::path primaryPreview;
    std::vector<std::string> tags;
    std::optional<std::uint64_t> publishedFileId;
    std::string packageId;
    std::string lastPublishedVersion;
    std::string lastPublishedSha256;
    std::string lastPublishedAt;
};

struct WorkshopMetadata
{
    std::string packageId;
    std::string version;
};

class ProjectStore
{
public:
    explicit ProjectStore(std::filesystem::path root);

    bool Load(std::string& error);
    bool Save(std::string& error) const;
    bool AddDirectory(const std::filesystem::path& source,
        WorkshopProject*& project, std::string& error);
    bool Discover(const std::filesystem::path& developmentRoot,
        std::size_t& added, std::string& error);
    bool Remove(std::string_view localId, std::string& error);

    std::vector<WorkshopProject>& Projects() { return projects_; }
    const std::vector<WorkshopProject>& Projects() const { return projects_; }
    const std::filesystem::path& Root() const { return root_; }
    std::filesystem::path StorePath() const;

private:
    std::filesystem::path root_;
    std::vector<WorkshopProject> projects_;
};

std::filesystem::path WorkshopManagerDataRoot(
    const std::filesystem::path& dataDirectory);
std::filesystem::path LegacyWorkshopManagerDataRoot();
bool MigrateWorkshopManagerData(const std::filesystem::path& legacyRoot,
    const std::filesystem::path& targetRoot, std::string& error);
bool MigrateWorkshopManagerDataOnce(
    const std::filesystem::path& targetRoot, std::string& error,
    std::optional<std::filesystem::path> legacyRoot = std::nullopt);

std::string BuildWorkshopMetadata(
    std::string_view packageId, std::string_view version);
std::optional<WorkshopMetadata> ParseWorkshopMetadata(
    std::string_view metadata, std::string& error);
bool CanBindWorkshopItem(const WorkshopProject& project,
    std::string_view metadata, std::uint64_t ownerSteamId,
    std::uint64_t currentSteamId, std::uint32_t consumerAppId,
    std::uint32_t currentAppId, std::string& error);
}
