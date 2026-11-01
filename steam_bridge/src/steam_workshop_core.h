// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "steam_connection_feedback.h"
#include "manager_connection_state.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

// Steam visibility values: public=0, friends=1, private=2, unlisted=3.
inline std::optional<int> ResolveWorkshopVisibility(bool creating,
    std::optional<int> requested) noexcept
{
    return creating ? std::optional<int>(requested.value_or(0)) : requested;
}

struct CoreError
{
    int exitCode = kSteamOperationFailed;
    std::string code;
    std::string message;
    std::optional<std::uint32_t> steamInitResult = {};
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
    ManagerConnectionState connectionState = ManagerConnectionState::Connecting;
    SteamConnectionProblem connectionProblem = SteamConnectionProblem::None;
    std::uint64_t connectionRevision = 0;
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
    std::vector<std::string> additionalPreviewFilenames;
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
    PreparingConfig,
    PreparingContent,
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

enum class WorkshopContentKind { Widget, Theme, IconPack };
struct PublishRequest
{
    WorkshopContentKind contentKind = WorkshopContentKind::Widget;
    std::string expectedPackageId;
    // Journal an uncertain creation only at the actual remote-create boundary.
    std::function<bool()> prepareCreateItem;
    // A newly allocated ID must be durably recorded before any upload starts.
    std::function<bool(std::uint64_t)> persistCreatedItem;
    std::function<bool(const std::filesystem::path&, const std::filesystem::path&)> validateStagedArtifacts;
    std::function<bool(const std::vector<std::filesystem::path>&)> validateStagedPreviews;
    std::vector<std::filesystem::path> additionalPreviews;
    // Only this theme's generated images may be replaced on an update.
    std::string managedPreviewPrefix;
    // Previous remote identity, verified from the owned item's metadata.
    std::string previousManagedPreviewPrefix;
    std::filesystem::path package;
    bool updateContent = true;
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
    explicit SteamWorkshopCore(std::filesystem::path stagingRoot = {});
    ~SteamWorkshopCore();
    SteamWorkshopCore(const SteamWorkshopCore&) = delete;
    SteamWorkshopCore& operator=(const SteamWorkshopCore&) = delete;

    bool Initialize(CoreError& error);
    // Call on the same serial Steam worker as all UGC operations, while idle.
    // Pump frequently; the implementation samples once per second and retries
    // transient initialization failures with bounded backoff. Returns whether
    // the API is initialized; inspect Status().loggedOn for backend connection.
    bool PollConnection(CoreError& error, bool retry = false);
    // Same worker, including a UGC progress callback. Samples only: never pumps
    // callbacks, releases a session, retries initialization or touches UGC.
    void SampleActiveConnection();
    void Shutdown();
    SteamStatus Status() const;
    std::optional<PublishedPage> ListPublished(
        std::uint32_t page, CoreError& error);
    std::optional<PublishedItem> FindItem(std::uint64_t id, CoreError& error);
    std::optional<WorkshopEulaStatus> GetEulaStatus(CoreError& error);
    bool SetSubscribed(std::uint64_t publishedFileId, bool subscribed,
        std::chrono::seconds timeout, CoreError& error);
    std::optional<PublishResult> Publish(const PublishRequest& request,
        const PublishProgressCallback& progress, CoreError& error);
    const std::filesystem::path& StagingRoot() const { return stagingRoot_; }

private:
    struct ConnectionCallbacks;
    bool CaptureConnection(CoreError& error);
    void StoreStatus(SteamStatus status);
    void ShutdownSession();
    std::filesystem::path stagingRoot_;
    mutable std::mutex statusMutex_;
    SteamStatus status_;
    std::unique_ptr<ConnectionCallbacks> connectionCallbacks_;
    ManagerConnectionSchedule connectionSchedule_;
    CoreError connectionError_;
    bool sessionInitialized_ = false;
};

std::string PublishStageName(PublishStage stage);
std::string CommunityItemUrl(std::uint64_t publishedFileId);
}
