// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../../steam_bridge/src/manager_connection_state.h"
#include "../../steam_bridge/src/steam_workshop_core.h"

#include <array>
#include <chrono>

template<typename Check>
void TestManagerConnectionState(Check&& check)
{
    using namespace snowdesktop::steam_bridge;
    using namespace std::chrono_literals;
    using State = ManagerConnectionState;
    using Problem = SteamConnectionProblem;
    using Schedule = ManagerConnectionSchedule;
    const Schedule::TimePoint start{};

    // Exercise the same cadence and backoff consumed by PollConnection. No
    // Steam process, user account, network or wall-clock sleeps are involved.
    Schedule retry;
    check(retry.ShouldInitialize(start),
        "a new manager attempts its first Steam connection immediately");
    retry.Failed(start, Problem::ClientUnavailable);
    check(!retry.ShouldInitialize(start + 50ms) &&
            !retry.ShouldInitialize(start + 999ms) &&
            retry.ShouldInitialize(start + 1s),
        "a missing Steam client backs off instead of initializing every pump");
    check(retry.ShouldInitialize(start + 50ms, true),
        "an explicit connection retry bypasses the automatic retry deadline");

    auto now = start + 1s;
    constexpr std::array delays{2s, 4s, 8s, 16s, 30s, 30s, 30s};
    for (const auto delay : delays)
    {
        retry.Failed(now, Problem::InitializationFailed);
        check(!retry.ShouldInitialize(now + delay - 1ms) &&
                retry.ShouldInitialize(now + delay),
            "successive transient failures use bounded exponential backoff");
        now += delay;
    }
    retry.Initialized(now);
    check(!retry.ShouldInitialize(now + 5min),
        "a healthy session is sampled rather than repeatedly initialized");
    retry.Failed(now, Problem::InterfaceUnavailable);
    check(retry.ShouldInitialize(now + 1s) &&
            !retry.ShouldInitialize(now + 999ms),
        "a recovered session resets backoff for a later interface failure");

    for (const auto problem : {Problem::SteamworksUnavailable,
            Problem::AppIdMismatch, Problem::ClientOutdated})
    {
        Schedule blocked;
        blocked.Failed(start, problem);
        check(!blocked.ShouldInitialize(start + 24h) &&
                blocked.ShouldInitialize(start + 50ms, true),
            "configuration failures wait for an explicit retry without spinning");
    }

    Schedule samples;
    check(samples.ShouldSample(start),
        "an existing Steam session receives an initial complete sample");
    samples.Initialized(start);
    check(!samples.ShouldSample(start + 50ms) &&
            !samples.ShouldSample(start + 999ms) &&
            samples.ShouldSample(start + 1s),
        "frequent callback pumps do not repeatedly sample unchanged identity");
    check(samples.ShouldSample(start + 50ms, true),
        "a connection callback requests an immediate sample between deadlines");
    samples.Sampled(start + 50ms);
    check(!samples.ShouldSample(start + 1s) &&
            samples.ShouldSample(start + 1050ms),
        "an event-driven sample establishes the next regular sample deadline");

    check(ResolveManagerConnectionState(true, false, false, Problem::None) ==
            State::Connecting,
        "before initialization the manager displays a connecting state");
    check(ResolveManagerConnectionState(true, true, true, Problem::None) ==
            State::Connected &&
            ResolveManagerConnectionState(true, true, false, Problem::Offline) ==
                State::Offline &&
            ResolveManagerConnectionState(true, true, true, Problem::None) ==
                State::Connected,
        "an initialized session can go offline and recover without reinitializing");
    check(ResolveManagerConnectionState(true, false, false,
                Problem::ClientUnavailable) == State::ClientUnavailable &&
            ResolveManagerConnectionState(false, false, false,
                Problem::SteamworksUnavailable) == State::Unavailable &&
            ResolveManagerConnectionState(true, false, false,
                Problem::AppIdMismatch) == State::Unavailable,
        "missing clients, SDK-free builds and identity mismatches stay distinct");
    check(!CanAutomaticallyRetrySteamConnection(Problem::Offline),
        "backend offline status waits for Steam reconnection without tearing down the session");
    check(PublishStageName(PublishStage::PreparingConfig) == "preparing" &&
            PublishStageName(PublishStage::PreparingContent) == "preparing",
        "distinct UI preparation stages preserve the existing CLI progress protocol");
}
