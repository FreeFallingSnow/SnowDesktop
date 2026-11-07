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

    std::filesystem::path executable;
    // 0: incompatible bridge, 1: locked, 2: unlocked, 3: unlocked with the room card shown.
    std::function<int()> availability;
    std::function<void(bool)> openSettings;
    std::function<void()> changed;
    std::function<std::optional<std::size_t>(const std::vector<std::string>&)> chooseFriend;
    std::unique_ptr<steam_widget_multiplayer::Service> service;
    std::vector<steam_widget_multiplayer::Frame> friends;
    std::unordered_map<std::uint64_t, std::string> lastState;
    std::unordered_map<std::uint64_t, std::uint64_t> sends;
    struct Participation
    {
        std::uint64_t owner;
        bool peerReady = false;
        bool peerLeft = false;
    };
    std::unordered_map<std::string, Participation> participants;
    std::unordered_map<std::string, bool> pendingAnnouncements;
    std::optional<std::pair<std::string, bool>> announcement;
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
    bool readSteamLaunch = true;
};
}
