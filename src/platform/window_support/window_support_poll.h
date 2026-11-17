#pragma once

#include "window_support_protocol.h"

#include <cstddef>
#include <cstring>
#include <span>

namespace snowdesktop::window_support::protocol
{
// Hello and non-Poll commands retain the legacy layout on every version.
inline constexpr std::uint32_t CompactPollVersion = 3;

struct PollRequest
{
    std::uint32_t magic;
    std::uint32_t version;
    Command command;
    std::uint32_t hostProcess;
    std::uint64_t sequence;
};

// All fixed response fields travel even without events. A Poll that has
// pointer events uses the full Response so the existing event layout stays intact.
struct PollResponse
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t sequence;
    CapabilityMask capabilities;
    std::uint32_t error;
    std::uint32_t asynchronousError;
    std::uint32_t succeeded;
    std::uint32_t buttons;
    std::uint32_t reserved;
    WindowIdentity animationTarget;
    std::uint64_t presentation;
    std::uint32_t animationActive;
    std::uint32_t direction;
    WindowIdentity externalMinimize;

    PollResponse() = default;
    explicit PollResponse(const Response& response) noexcept
        : magic(response.magic), version(response.version), sequence(response.sequence),
          capabilities(response.capabilities), error(response.error), asynchronousError(response.asynchronousError),
          succeeded(response.succeeded), buttons(response.buttons), reserved(0), animationTarget(response.animationTarget),
          presentation(response.presentation), animationActive(response.animationActive), direction(response.direction),
          externalMinimize(response.externalMinimize) {}
};

static_assert(sizeof(PollRequest) == 24 && sizeof(PollResponse) == 88);
static_assert(std::is_trivially_copyable_v<PollRequest> && std::is_trivially_copyable_v<PollResponse>);

inline bool UsesCompactPoll(std::uint32_t version, Command command) noexcept
{
    return version == CompactPollVersion && command == Command::Poll;
}

inline bool DecodeRequest(std::span<const std::byte> bytes, std::uint32_t version, Request& request) noexcept
{
    if (bytes.size() == sizeof(Request))
    {
        std::memcpy(&request, bytes.data(), bytes.size());
        return !UsesCompactPoll(version, request.command);
    }
    if (version != CompactPollVersion || bytes.size() != sizeof(PollRequest)) return false;
    PollRequest poll;
    std::memcpy(&poll, bytes.data(), bytes.size());
    if (poll.command != Command::Poll) return false;
    request = {};
    request.magic = poll.magic;
    request.version = poll.version;
    request.command = poll.command;
    request.hostProcess = poll.hostProcess;
    request.sequence = poll.sequence;
    return true;
}

inline bool DecodePollResponse(std::span<const std::byte> bytes, Response& response) noexcept
{
    if (bytes.size() == sizeof(Response))
    {
        std::memcpy(&response, bytes.data(), bytes.size());
        return response.pointerCount != 0 && response.pointerCount <= response.pointerEvents.size();
    }
    if (bytes.size() != sizeof(PollResponse)) return false;
    PollResponse poll;
    std::memcpy(&poll, bytes.data(), bytes.size());
    if (poll.reserved != 0) return false;
    response = {};
    response.magic = poll.magic;
    response.version = poll.version;
    response.sequence = poll.sequence;
    response.capabilities = poll.capabilities;
    response.error = poll.error;
    response.asynchronousError = poll.asynchronousError;
    response.succeeded = poll.succeeded;
    response.buttons = poll.buttons;
    response.animationTarget = poll.animationTarget;
    response.presentation = poll.presentation;
    response.animationActive = poll.animationActive;
    response.direction = poll.direction;
    response.externalMinimize = poll.externalMinimize;
    return true;
}
}
