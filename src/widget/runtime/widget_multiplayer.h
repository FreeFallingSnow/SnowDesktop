#pragma once

#include "steam/steam_widget_multiplayer.h"
#include "steam/steam_room_status.h"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
struct WidgetMultiplayer
{
    using Clock = std::chrono::steady_clock;
    using Kind = steam_widget_multiplayer::Kind;
    static constexpr int kUnlocked = 2;
    static constexpr int kBackgroundListening = 4;
    static constexpr int kReportRunningStatus = 8;

    std::filesystem::path executable;
    // 0: incompatible bridge, 1: locked, otherwise kUnlocked with optional flags.
    std::function<int()> availability;
    std::function<void(bool)> openSettings;
    std::function<void()> changed;
    std::function<std::optional<std::size_t>(const std::vector<std::string>&)> chooseFriend;
    std::unique_ptr<steam_widget_multiplayer::Service> service;
    std::vector<steam_widget_multiplayer::Frame> friends;
    std::unordered_map<std::uint64_t, std::string> lastState;
    std::unordered_map<std::uint64_t, std::uint64_t> sends;
    enum class ParticipationSignal : char { Ready = '+', Withdraw = '-', Query = '?' };
    struct Participation
    {
        std::uint64_t owner;
        bool peerReady = false;
        bool peerLeft = false;
        Clock::time_point queryAfter{};
    };
    std::unordered_map<std::string, Participation> participants;
    std::unordered_map<std::string, ParticipationSignal> pendingAnnouncements;
    std::optional<std::pair<std::string, ParticipationSignal>> announcement;
    Clock::time_point announceAfter{};
    std::optional<steam_widget_multiplayer::Frame> invitation;
    Clock::time_point invitationUntil{}, launchUntil{}, retryAfter{};
    std::uint64_t launchLobby = 0;
    std::optional<Kind> operation;
    steam_room::Status published;
    steam_room::Failure failure = steam_room::Failure::None;
    int members = 0;
    bool host = false;
    bool ready = false;
    bool settingsListening = false;
    bool launchSubmitted = false;
    bool readSteamLaunch = true;
};
}
