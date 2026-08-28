// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::steam_bridge
{
inline constexpr int kSteamworksUnavailable = 3;
inline constexpr int kSteamInitializationFailed = 4;
inline constexpr int kSteamOperationFailed = 5;
inline constexpr int kSteamOperationTimedOut = 6;
inline constexpr int kInvalidArguments = 64;

struct CoreError
{
    int exitCode = kSteamOperationFailed;
    std::string code;
    std::string message;
};

struct SteamStatus
{
    bool compiled = false;
    bool initialized = false;
    bool loggedOn = false;
    std::uint32_t expectedAppId = 0;
    std::uint32_t appId = 0;
    std::string steamId;
    std::string diagnostic;
};

struct WorkshopEulaStatus
{
    bool available = false;
    bool accepted = false;
    bool needsAction = false;
    std::uint32_t version = 0;
    std::uint32_t actionTime = 0;
};

struct PublishedItem
{
    std::uint64_t publishedFileId = 0;
    std::uint64_t ownerSteamId = 0;
    std::uint32_t creatorAppId = 0;
    std::uint32_t consumerAppId = 0;
    std::uint32_t createdAt = 0;
    std::uint32_t updatedAt = 0;
    int visibility = 0;
    bool banned = false;
    bool acceptedForUse = false;
    std::string title;
    std::string description;
    std::string metadata;
    std::string previewUrl;
    std::vector<std::string> tags;
    std::uint64_t subscriptions = 0;
    std::uint64_t favorites = 0;
    std::uint64_t views = 0;
    std::uint64_t comments = 0;
    std::uint64_t fileSize = 0;
    float score = 0.0f;
};

struct PublishedPage
{
    std::uint32_t page = 1;
    std::uint32_t totalPages = 0;
    std::uint32_t totalResults = 0;
    std::vector<PublishedItem> items;
};

enum class PublishStage
{
    Preparing,
    Created,
    UploadingContent,
    UploadingPreview,
    CommittingChanges,
    Completed,
};

struct PublishProgress
{
    PublishStage stage = PublishStage::Preparing;
    std::uint64_t publishedFileId = 0;
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
    int steamStatus = 0;
    bool submitStarted = false;
};

struct PublishRequest
{
    std::filesystem::path package;
    std::optional<std::filesystem::path> preview;
    std::optional<std::uint64_t> publishedFileId;
    std::string title;
    std::optional<std::string> description;
    std::optional<std::string> language;
    std::optional<std::string> metadata;
    std::optional<std::vector<std::string>> tags;
    std::optional<int> visibility;
    std::string changeNote;
    std::chrono::seconds timeout = std::chrono::minutes(30);
};

struct PublishResult
{
    bool created = false;
    std::uint64_t publishedFileId = 0;
    bool needsLegalAgreement = false;
    std::string communityUrl;
};

using PublishProgressCallback = std::function<void(const PublishProgress&)>;

class SteamWorkshopCore
{
public:
    SteamWorkshopCore();
    ~SteamWorkshopCore();
    SteamWorkshopCore(const SteamWorkshopCore&) = delete;
    SteamWorkshopCore& operator=(const SteamWorkshopCore&) = delete;

    bool Initialize(CoreError& error);
    SteamStatus Status() const;
    std::optional<PublishedPage> ListPublished(
        std::uint32_t page, CoreError& error);
    std::optional<WorkshopEulaStatus> GetEulaStatus(CoreError& error);
    bool SetSubscribed(std::uint64_t publishedFileId, bool subscribed,
        std::chrono::seconds timeout, CoreError& error);
    std::optional<PublishResult> Publish(const PublishRequest& request,
        const PublishProgressCallback& progress, CoreError& error);

private:
    mutable std::mutex statusMutex_;
    SteamStatus status_;
};

std::string PublishStageName(PublishStage stage);
std::string CommunityItemUrl(std::uint64_t publishedFileId);
}
