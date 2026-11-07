#pragma once

#include "steam/steam_workshop_sync.h"
#include "winui/widgets_page_backend.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SteamWorkshopSubscriptionPollState {
    struct ReadyQuery
    {
        std::uint64_t queryId = 0;
        snowdesktop::widget::SteamWorkshopSubscriptionSnapshot snapshot;
    };
    struct SettingsCompletion
    {
        std::uint64_t generation = 0;
        std::uint64_t taskId = 0;
        std::uint64_t queryId = 0;
        std::string expectedUnsubscribedPublishedFileId;
        std::vector<std::string> expectedRemovedPackageIds;
        // Starts on the first pending reconciliation, after any host deferral.
        std::chrono::steady_clock::time_point reconciliationDeadline{};
        snowdesktop::winui::WidgetsPageBackendOptions::AsyncCompletion done;
    };
    std::atomic<bool> queryInFlight{ false };
    std::atomic<bool> refreshPending{ false };
    std::mutex mutex;
    std::optional<ReadyQuery> ready;
    std::vector<SettingsCompletion> settingsCompletions;
    std::uint64_t nextQueryId = 1;
    std::uint64_t activeQueryId = 0;
};
