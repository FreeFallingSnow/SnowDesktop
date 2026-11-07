#pragma once

#include <cstdint>

namespace snowdesktop::steam_room
{
enum class Command : std::uint8_t { Query, Listen, StopListening, Create, Invite, Join, Leave };
enum class Phase : std::uint8_t { Unavailable, Locked, Idle, Invited, Waiting, Connected };
enum class Failure : std::uint8_t { None, SteamUnavailable, ActionFailed, NoFriends };

struct Status
{
    Phase phase = Phase::Unavailable;
    Failure failure = Failure::None;
    bool busy = false;
    bool inRoom = false;
    bool host = false;

    bool operator==(const Status&) const = default;
};
}
