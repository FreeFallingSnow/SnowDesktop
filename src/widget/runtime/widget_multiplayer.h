#pragma once

#include "steam/steam_widget_multiplayer.h"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
struct WidgetMultiplayer
{
    using Clock = std::chrono::steady_clock;
    using Kind = steam_widget_multiplayer::Kind;

    std::filesystem::path executable;
    // 0: incompatible bridge, 1: locked, 2: unlocked, 3: unlocked with Steam presence.
    std::function<int()> availability;
    std::function<void()> unlock;
    std::function<std::optional<std::size_t>(const std::vector<std::string>&)> chooseFriend;
    std::unique_ptr<steam_widget_multiplayer::Service> service;
    std::vector<steam_widget_multiplayer::Frame> friends;
    std::unordered_map<std::uint64_t, std::string> lastState;
    std::optional<steam_widget_multiplayer::Frame> invitation;
    Clock::time_point invitationUntil{}, listenUntil{}, retryAfter{};
    std::uint64_t launchLobby = 0;
    std::uint64_t owner = 0;
    std::uint64_t task = 0;
    std::uint64_t taskOwner = 0;
    Kind operation{};
    int members = 0;
    bool host = false;
    bool ready = false;
    bool readSteamLaunch = true;
};
}
