// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "steam_connection_feedback.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace snowdesktop::steam_bridge
{
enum class ManagerConnectionState : std::uint8_t
{
    Connecting,
    Connected,
    Offline,
    ClientUnavailable,
    Unavailable,
};

constexpr ManagerConnectionState ResolveManagerConnectionState(bool compiled,
    bool initialized, bool loggedOn, SteamConnectionProblem problem) noexcept
{
    if (!compiled) return ManagerConnectionState::Unavailable;
    if (initialized)
        return loggedOn ? ManagerConnectionState::Connected :
            ManagerConnectionState::Offline;
    if (problem == SteamConnectionProblem::ClientUnavailable)
        return ManagerConnectionState::ClientUnavailable;
    return problem == SteamConnectionProblem::None ?
        ManagerConnectionState::Connecting : ManagerConnectionState::Unavailable;
}

constexpr bool CanAutomaticallyRetrySteamConnection(
    SteamConnectionProblem problem) noexcept
{
    return problem == SteamConnectionProblem::ClientUnavailable ||
        problem == SteamConnectionProblem::InitializationFailed ||
        problem == SteamConnectionProblem::InterfaceUnavailable;
}

// Steam API ownership belongs to the caller's serial worker. This schedule only
// decides when to sample or initialize; it never retries a remote UGC operation.
class ManagerConnectionSchedule
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto kSampleInterval = std::chrono::seconds(1);

    bool ShouldInitialize(TimePoint now, bool manualRetry = false) const noexcept
    {
        return manualRetry || !attempted_ || now >= nextAttempt_;
    }

    bool ShouldSample(TimePoint now, bool requested = false) const noexcept
    {
        return requested || !sampled_ || now >= nextSample_;
    }

    void Sampled(TimePoint now) noexcept
    {
        sampled_ = true;
        nextSample_ = now + kSampleInterval;
    }

    void Initialized(TimePoint now) noexcept
    {
        attempted_ = true;
        failures_ = 0;
        nextAttempt_ = TimePoint::max();
        Sampled(now);
    }

    void Failed(TimePoint now, SteamConnectionProblem problem) noexcept
    {
        attempted_ = true;
        if (!CanAutomaticallyRetrySteamConnection(problem))
        {
            nextAttempt_ = TimePoint::max();
            return;
        }
        const unsigned shift = std::min(failures_, 5u);
        const auto delay = std::chrono::seconds(std::min(1u << shift, 30u));
        failures_ = std::min(failures_ + 1u, 6u);
        nextAttempt_ = now + delay;
    }

private:
    bool attempted_ = false;
    bool sampled_ = false;
    unsigned failures_ = 0;
    TimePoint nextAttempt_{};
    TimePoint nextSample_{};
};
}
