// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../../steam_bridge/src/manager_publish_task.h"
#include "../../steam_bridge/src/manager_prepared_preview.h"
#include "../test_temporary_directory.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

inline void TestManagerPublishTask()
{
    using namespace snowdesktop::steam_bridge;
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    const auto progress = [](std::size_t index, std::size_t count,
        std::string language, PublishStage stage, std::uint64_t processed = 0,
        std::uint64_t total = 0, bool submitting = true) {
        return ComponentPublishProgress{index, count, std::move(language),
            PublishProgress{stage, 700, processed, total, 0, submitting}};
    };

    ManagerPublishTask task;
    require(!task.Snapshot().HasTask(), "a new publish task has no retained result");
    task.Begin("project-a", "Original component");
    require(task.Snapshot().Running() && !task.Snapshot().StageFraction(),
        "preparation starts with an unknown current-stage total");
    task.Update(progress(1, 1, "english", PublishStage::UploadingContent, 64, 128));
    require(task.Snapshot().StageFraction() == 0.5f &&
        task.Snapshot().confirmedSubmissions == 0 && task.Snapshot().submitStarted,
        "real upload bytes describe only the current unconfirmed stage");
    task.Update(progress(1, 1, "english", PublishStage::UploadingContent, 128, 128));
    task.Update(progress(1, 1, "english", PublishStage::Completed));
    require(task.Snapshot().StageFraction() == 1.0f &&
        task.Snapshot().confirmedSubmissions == 1 &&
        task.Snapshot().confirmedLanguages == 1 && task.Snapshot().Running(),
        "Completed with zero counters confirms the submission without returning to zero");
    task.Update(progress(1, 1, "english", PublishStage::Completed));
    require(task.Snapshot().confirmedSubmissions == 1 &&
        task.Snapshot().confirmedLanguages == 1,
        "duplicate completion callbacks never inflate confirmed counts");
    ComponentPublishResult result;
    result.baseSubmitted = true;
    result.contentUploaded = true;
    result.publishedFileId = 700;
    result.localizedSubmissions = 1;
    result.communityUrl = "https://steamcommunity.com/sharedfiles/filedetails/?id=700";
    task.Finish(result, true, {});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Completed &&
        task.Snapshot().StageFraction() == 1.0f &&
        !task.Snapshot().submitStarted && task.Snapshot().contentUploaded,
        "the terminal result retains successful completion and releases submit protection");
    task.Update(progress(1, 1, "english", PublishStage::Preparing));
    require(task.Snapshot().outcome == ManagerPublishOutcome::Completed &&
        task.Snapshot().stage == PublishStage::Completed,
        "late progress cannot overwrite a retained terminal result");

    task.Begin("project-a", "Original component");
    task.Update(progress(1, 3, "english", PublishStage::UploadingContent, 9, 10));
    require(task.Snapshot().StageFraction() == 0.9f,
        "the first large upload is not divided by the number of languages");
    task.Update(progress(1, 3, "english", PublishStage::Completed));
    task.Update(progress(2, 3, "schinese", PublishStage::Preparing, 0, 0, false));
    require(!task.Snapshot().StageFraction() &&
        task.Snapshot().confirmedLanguages == 1 &&
        task.Snapshot().submissionIndex == 2 &&
        task.Snapshot().localId == "project-a" &&
        task.Snapshot().name == "Original component",
        "the next language has unknown work while task identity and confirmed work persist");
    task.Update(progress(1, 3, "english", PublishStage::UploadingContent, 0, 10));
    require(task.Snapshot().submissionIndex == 2 &&
        task.Snapshot().language == "schinese",
        "a delayed event cannot relabel the current submission");
    task.Update(progress(2, 3, "schinese", PublishStage::Completed));
    task.Update(progress(3, 3, "japanese", PublishStage::CommittingChanges));
    result.localizedSubmissions = 2;
    result.failedLanguage = "japanese";
    task.Finish(result, false,
        CoreError{kSteamOperationFailed, "publish_failed", "access_denied"});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Partial &&
        task.Snapshot().baseSubmitted && task.Snapshot().contentUploaded &&
        task.Snapshot().confirmedLanguages == 2 &&
        task.Snapshot().failedLanguage == "japanese" &&
        task.Snapshot().publishedFileId == 700 &&
        task.Snapshot().localId == "project-a",
        "a later language failure retains the confirmed content and original project owner");

    ComponentPublishResult primaryConfirmed;
    primaryConfirmed.baseSubmitted = true;
    primaryConfirmed.contentUploaded = true;
    primaryConfirmed.publishedFileId = 700;
    primaryConfirmed.localizedSubmissions = 1;
    primaryConfirmed.failedLanguage = "schinese";
    task.Begin("preflight-after-success", "Confirmed primary upload");
    task.Update(progress(1, 2, "english", PublishStage::UploadingContent, 10, 10));
    task.Update(progress(1, 2, "english", PublishStage::Completed));
    task.Update(progress(2, 2, "schinese", PublishStage::Preparing, 0, 0, false));
    task.Finish(primaryConfirmed, false,
        CoreError{kSteamOperationTimedOut, "query_timed_out", "Second language preflight"});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Partial &&
        task.Snapshot().baseSubmitted && task.Snapshot().contentUploaded &&
        task.Snapshot().confirmedSubmissions == 1 &&
        task.Snapshot().confirmedLanguages == 1 &&
        task.Snapshot().failedLanguage == "schinese" &&
        task.Snapshot().localId == "preflight-after-success",
        "a later language preflight timeout preserves confirmed work without inventing an uncertain submit");

    task.Begin("second-submit-timeout", "Unconfirmed second language");
    task.Update(progress(1, 2, "english", PublishStage::UploadingContent, 10, 10));
    task.Update(progress(1, 2, "english", PublishStage::Completed));
    task.Update(progress(2, 2, "schinese", PublishStage::Preparing, 0, 0, false));
    task.Update(progress(2, 2, "schinese", PublishStage::CommittingChanges));
    task.Finish(primaryConfirmed, false,
        CoreError{kSteamOperationTimedOut, "publish_timed_out", "Second language callback"});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Unknown &&
        task.Snapshot().baseSubmitted && task.Snapshot().contentUploaded &&
        task.Snapshot().confirmedSubmissions == 1 &&
        task.Snapshot().confirmedLanguages == 1 &&
        task.Snapshot().failedLanguage == "schinese" &&
        task.Snapshot().localId == "second-submit-timeout",
        "a timeout after the next submit starts distinguishes its unknown outcome from the confirmed primary upload");

    task.Begin("metadata-project", "Listing update");
    task.Update(progress(1, 1, {}, PublishStage::CommittingChanges));
    require(!task.Snapshot().StageFraction(),
        "a metadata-only update has no fabricated upload percentage");
    task.Update(progress(1, 1, {}, PublishStage::Completed));
    result = {};
    result.baseSubmitted = true;
    result.publishedFileId = 700;
    task.Finish(result, true, {});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Completed &&
        !task.Snapshot().contentUploaded &&
        task.Snapshot().confirmedLanguages == 0 &&
        task.Snapshot().confirmedSubmissions == 1,
        "metadata-only completion confirms a submission without inventing a content upload or language");

    task.Begin("uncertain-project", "Waiting for Steam");
    task.Update(progress(1, 1, "english", PublishStage::Preparing, 0, 0, true));
    require(task.Snapshot().submitStarted && !task.Snapshot().StageFraction() &&
        task.Snapshot().confirmedSubmissions == 0,
        "a newly started submit has unknown work rather than a fabricated zero percent");
    task.Update(progress(1, 1, "english", PublishStage::PreparingConfig));
    require(task.Snapshot().stage == PublishStage::PreparingConfig &&
        !task.Snapshot().StageFraction(),
        "Steam configuration preparation retains its real phase with unknown work");
    task.Update(progress(1, 1, "english", PublishStage::PreparingContent));
    require(task.Snapshot().stage == PublishStage::PreparingContent &&
        !task.Snapshot().StageFraction(),
        "Steam content preparation does not pretend to be a committed upload");
    task.Update(progress(1, 1, "english", PublishStage::CommittingChanges));
    task.Finish({}, false,
        CoreError{kSteamOperationTimedOut, "publish_timed_out", "No callback"});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Unknown &&
        task.Snapshot().publishedFileId == 700 &&
        task.Snapshot().confirmedSubmissions == 0 &&
        !task.Snapshot().submitStarted,
        "a submit timeout preserves the item ID and leaves the remote outcome unconfirmed");

    task.Begin("interrupted-project", "Interrupted submit");
    task.Update(progress(1, 1, "english", PublishStage::Preparing, 0, 0, true));
    task.Finish({}, false,
        CoreError{kSteamOperationFailed, "publish_interrupted", "Transport exception"});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Unknown &&
        !task.Snapshot().submitStarted &&
        task.Snapshot().localId == "interrupted-project" &&
        task.Snapshot().name == "Interrupted submit" &&
        task.Snapshot().publishedFileId == 700,
        "an exception after submit releases close protection without claiming the remote update failed");

    task.Begin("pre-submit-project", "Interrupted preparation");
    task.Update(progress(1, 1, "english", PublishStage::Preparing, 0, 0, false));
    task.Finish({}, false,
        CoreError{kSteamOperationFailed, "publish_interrupted", "Preparation exception"});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Failed &&
        !task.Snapshot().submitStarted &&
        task.Snapshot().localId == "pre-submit-project" &&
        task.Snapshot().name == "Interrupted preparation",
        "an exception before submit retains task identity and reports a definite local failure");

    task.Begin("offline-project", "Offline preparation");
    task.Finish({}, false,
        CoreError{kSteamOperationTimedOut, "query_timed_out", "Before submission"});
    require(task.Snapshot().outcome == ManagerPublishOutcome::Failed &&
        task.Snapshot().errorCode == "query_timed_out" &&
        task.Snapshot().confirmedSubmissions == 0,
        "a pre-submit failure does not claim an uncertain remote publication");

    task.Begin("legal-project", "Agreement needed");
    result = {};
    result.baseSubmitted = true;
    result.publishedFileId = 700;
    result.needsLegalAgreement = true;
    task.Finish(result, true, {});
    require(task.Snapshot().outcome == ManagerPublishOutcome::NeedsLegalAgreement &&
        task.Snapshot().needsLegalAgreement && task.Snapshot().baseSubmitted,
        "a successful upload requiring agreement has a distinct retained outcome");

    task.Begin("replacement-project", "A new task");
    require(task.Snapshot().localId == "replacement-project" &&
        task.Snapshot().name == "A new task" &&
        task.Snapshot().confirmedLanguages == 0 &&
        task.Snapshot().publishedFileId == 0 &&
        task.Snapshot().errorCode.empty() && task.Snapshot().Running(),
        "only a new task explicitly replaces the retained task identity and result");

    const auto start = ManagerPublishTask::Clock::time_point{} +
        std::chrono::seconds(100);
    task.Begin("timed-project", "Measured wait", start);
    require(task.Snapshot().startedAt == start &&
        task.Snapshot().StageElapsed(start + std::chrono::seconds(3)).count() == 3,
        "task timing uses a supplied monotonic start without sleeping");
    task.Update(progress(1, 2, "english", PublishStage::CommittingChanges),
        start + std::chrono::seconds(4));
    task.Update(progress(1, 2, "english", PublishStage::CommittingChanges),
        start + std::chrono::seconds(6));
    require(task.Snapshot().stageStartedAt == start + std::chrono::seconds(4) &&
        task.Snapshot().StageElapsed(start + std::chrono::seconds(9)).count() == 5 &&
        task.Snapshot().startedAt == start,
        "repeated samples do not reset waiting time or the complete task start");
    task.Update(progress(1, 2, "english", PublishStage::Completed),
        start + std::chrono::seconds(10));
    require(task.Snapshot().stageStartedAt == start + std::chrono::seconds(10),
        "a different Steam stage begins its own measured interval");
    task.Update(progress(2, 2, "schinese", PublishStage::Completed),
        start + std::chrono::seconds(12));
    require(task.Snapshot().stageStartedAt == start + std::chrono::seconds(12) &&
        task.Snapshot().StageElapsed(start + std::chrono::seconds(14)).count() == 2 &&
        task.Snapshot().StageElapsed(start).count() == 0,
        "a new language resets the interval even with the same stage and clamps earlier observations");

    const auto writePreviewTestFile = [&](const std::filesystem::path& path,
        std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(output), "preview fixture writes its owned bytes");
    };
    const auto readPreviewTestFile = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };
    {
        snowdesktop::test::TemporaryDirectory temporary;
        const auto source = temporary.path / L"original.PNG";
        const auto artifacts = temporary.path / L"artifacts";
        std::filesystem::create_directory(artifacts);
        writePreviewTestFile(source, "preview before review");
        std::string error;
        const auto frozen = FreezeManagerPreview(source, artifacts, error);
        require(frozen && error.empty() &&
            frozen->Path() == artifacts / L"prepared-preview.png",
            "the real preview factory creates an owned snapshot with a legal normalized extension");
        writePreviewTestFile(source, "preview replaced at the same original path");
        require(readPreviewTestFile(frozen->Path()) == "preview before review" &&
            readPreviewTestFile(source) == "preview replaced at the same original path",
            "rewriting the original cover after preparation cannot change the actual frozen upload bytes");
    }
    {
        snowdesktop::test::TemporaryDirectory temporary;
        const auto source = temporary.path / L"original.png";
        const auto artifacts = temporary.path / L"artifacts";
        std::filesystem::create_directory(artifacts);
        const auto package = artifacts / L"package.snowwidget";
        const auto unrelated = artifacts / L"another-owned-file.txt";
        writePreviewTestFile(source, "original preview");
        writePreviewTestFile(package, "package retained");
        writePreviewTestFile(unrelated, "unrelated file retained");
        std::string error;
        auto firstOwner = FreezeManagerPreview(source, artifacts, error);
        require(firstOwner && error.empty(), "cleanup fixture freezes a real preview file");
        const auto frozenPath = firstOwner->Path();
        auto lastOwner = firstOwner;
        firstOwner.reset();
        require(std::filesystem::is_regular_file(frozenPath),
            "a preview remains available while a prepared plan still shares its owner");
        lastOwner.reset();
        require(!std::filesystem::exists(frozenPath) &&
            readPreviewTestFile(package) == "package retained" &&
            readPreviewTestFile(unrelated) == "unrelated file retained" &&
            readPreviewTestFile(source) == "original preview" &&
            std::filesystem::is_directory(artifacts),
            "the final preview owner deletes only its exact file and leaves the package, siblings and source intact");
    }
}
