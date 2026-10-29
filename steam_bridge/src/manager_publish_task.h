// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component_workshop_publish.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::steam_bridge
{
enum class ManagerPublishOutcome
{
    None,
    Running,
    Completed,
    Partial,
    Unknown,
    Failed,
    NeedsLegalAgreement,
};

struct ManagerPublishTaskSnapshot
{
    using Clock = std::chrono::steady_clock;

    std::string localId;
    std::string name;
    std::string language;
    std::string failedLanguage;
    std::string errorCode;
    std::string errorMessage;
    std::string communityUrl;
    PublishStage stage = PublishStage::Preparing;
    ManagerPublishOutcome outcome = ManagerPublishOutcome::None;
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
    std::uint64_t publishedFileId = 0;
    std::size_t submissionIndex = 0;
    std::size_t submissionTotal = 0;
    std::size_t confirmedSubmissions = 0;
    std::size_t confirmedLanguages = 0;
    bool submitStarted = false;
    bool baseSubmitted = false;
    bool contentUploaded = false;
    bool needsLegalAgreement = false;
    bool stageComplete = false;
    Clock::time_point startedAt{};
    Clock::time_point stageStartedAt{};

    bool HasTask() const { return outcome != ManagerPublishOutcome::None; }
    bool Running() const { return outcome == ManagerPublishOutcome::Running; }

    std::chrono::seconds StageElapsed(
        Clock::time_point now = Clock::now()) const
    {
        if (!HasTask() || now <= stageStartedAt) return std::chrono::seconds::zero();
        return std::chrono::duration_cast<std::chrono::seconds>(now - stageStartedAt);
    }

    // This is only the current Steam stage's fraction, never an overall
    // publication percentage. A successful callback completes a stage even
    // though its terminal event has no byte counters.
    std::optional<float> StageFraction() const
    {
        if (stageComplete) return 1.0f;
        if (total == 0) return std::nullopt;
        return static_cast<float>(std::clamp(
            static_cast<double>(processed) / static_cast<double>(total),
            0.0, 1.0));
    }
};

// The manager owns synchronization. Apply events and copy/read Snapshot under
// the same manager mutex so task identity, counters and outcome form one view.
class ManagerPublishTask
{
public:
    using Clock = ManagerPublishTaskSnapshot::Clock;

    void Begin(std::string localId, std::string name,
        Clock::time_point now = Clock::now())
    {
        snapshot_ = {};
        snapshot_.localId = std::move(localId);
        snapshot_.name = std::move(name);
        snapshot_.outcome = ManagerPublishOutcome::Running;
        snapshot_.startedAt = now;
        snapshot_.stageStartedAt = now;
        confirmedIndices_.clear();
        hasSubmitted_ = false;
    }

    void Update(const ComponentPublishProgress& progress,
        Clock::time_point now = Clock::now())
    {
        if (!snapshot_.Running() || progress.submissionIndex == 0 ||
            progress.submissionTotal == 0 ||
            progress.submissionIndex > progress.submissionTotal)
            return;
        if (snapshot_.submissionTotal != 0 &&
            snapshot_.submissionTotal != progress.submissionTotal)
            return;
        // A delayed event from an earlier submission must not replace the
        // current language or put the task back into an earlier stage.
        if (progress.submissionIndex < snapshot_.submissionIndex) return;

        const auto& steam = progress.steam;
        if (progress.submissionIndex != snapshot_.submissionIndex ||
            steam.stage != snapshot_.stage)
            snapshot_.stageStartedAt = now;
        snapshot_.submissionIndex = progress.submissionIndex;
        snapshot_.submissionTotal = progress.submissionTotal;
        snapshot_.language = progress.language;
        snapshot_.stage = steam.stage;
        snapshot_.processed = steam.processed;
        snapshot_.total = steam.total;
        snapshot_.submitStarted = steam.submitStarted;
        snapshot_.stageComplete = steam.stage == PublishStage::Completed;
        hasSubmitted_ = hasSubmitted_ || steam.submitStarted;
        if (steam.publishedFileId != 0)
            snapshot_.publishedFileId = steam.publishedFileId;

        if (snapshot_.stageComplete &&
            std::find(confirmedIndices_.begin(), confirmedIndices_.end(),
                progress.submissionIndex) == confirmedIndices_.end())
        {
            confirmedIndices_.push_back(progress.submissionIndex);
            snapshot_.confirmedSubmissions = confirmedIndices_.size();
            if (!progress.language.empty()) ++snapshot_.confirmedLanguages;
            if (progress.submissionIndex == 1) snapshot_.baseSubmitted = true;
        }
    }

    void Finish(const ComponentPublishResult& result, bool succeeded,
        const CoreError& error)
    {
        if (!snapshot_.Running()) return;
        snapshot_.submitStarted = false;
        snapshot_.baseSubmitted = snapshot_.baseSubmitted || result.baseSubmitted;
        snapshot_.contentUploaded = result.contentUploaded;
        snapshot_.needsLegalAgreement = result.needsLegalAgreement;
        snapshot_.failedLanguage = result.failedLanguage;
        snapshot_.communityUrl = result.communityUrl;
        if (result.publishedFileId != 0)
            snapshot_.publishedFileId = result.publishedFileId;

        const std::size_t knownLanguages = snapshot_.submissionTotal == 0 ?
            result.localizedSubmissions :
            std::min(result.localizedSubmissions, snapshot_.submissionTotal);
        snapshot_.confirmedLanguages = std::max(
            snapshot_.confirmedLanguages, knownLanguages);
        snapshot_.confirmedSubmissions = std::max(
            snapshot_.confirmedSubmissions, snapshot_.baseSubmitted ?
                std::max<std::size_t>(1, knownLanguages) : knownLanguages);

        if (succeeded)
        {
            snapshot_.stage = PublishStage::Completed;
            snapshot_.stageComplete = true;
            if (snapshot_.submissionTotal != 0)
                snapshot_.confirmedSubmissions = snapshot_.submissionTotal;
            snapshot_.outcome = result.needsLegalAgreement ?
                ManagerPublishOutcome::NeedsLegalAgreement :
                ManagerPublishOutcome::Completed;
            return;
        }

        snapshot_.errorCode = error.code;
        snapshot_.errorMessage = error.message;
        if (result.needsLegalAgreement &&
            (error.code == "agreementRequired" || error.code.empty()))
            snapshot_.outcome = ManagerPublishOutcome::NeedsLegalAgreement;
        else if (error.code == "publish_timed_out" ||
            (hasSubmitted_ && error.code == "publish_interrupted") ||
            (hasSubmitted_ && error.exitCode == kSteamOperationTimedOut))
            snapshot_.outcome = ManagerPublishOutcome::Unknown;
        else if (snapshot_.baseSubmitted || snapshot_.confirmedSubmissions != 0)
            snapshot_.outcome = ManagerPublishOutcome::Partial;
        else
            snapshot_.outcome = ManagerPublishOutcome::Failed;
    }

    const ManagerPublishTaskSnapshot& Snapshot() const { return snapshot_; }

private:
    ManagerPublishTaskSnapshot snapshot_;
    std::vector<std::size_t> confirmedIndices_;
    bool hasSubmitted_ = false;
};
}
