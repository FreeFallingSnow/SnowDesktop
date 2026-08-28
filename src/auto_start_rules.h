#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace snowdesktop
{

enum class PortableAutoStartApprovalState : std::uint8_t
{
    Missing,
    Enabled,
    Disabled,
    Error,
};

enum class PortableAutoStartRegistrationOwner : std::uint8_t
{
    Missing,
    CurrentExecutable,
    OtherExecutable,
    Error,
};

[[nodiscard]] constexpr PortableAutoStartApprovalState
DecodePortableAutoStartApprovalState(std::uint8_t marker) noexcept
{
    switch (marker)
    {
    case 0x00:
        // Task Manager can leave a zeroed 12-byte approval value after a
        // classic desktop startup app is disabled. The Run registration is
        // retained, but Windows does not launch it.
    case 0x01:
        // Current Windows builds may write 0x01 plus a FILETIME after a
        // classic desktop startup app is disabled in Windows Settings.
    case 0x03:
        return PortableAutoStartApprovalState::Disabled;
    case 0x02:
        return PortableAutoStartApprovalState::Enabled;
    default:
        return PortableAutoStartApprovalState::Error;
    }
}

inline constexpr std::size_t kPortableAutoStartApprovalPayloadSize = 12;

[[nodiscard]] constexpr std::array<std::uint8_t,
    kPortableAutoStartApprovalPayloadSize>
BuildPortableAutoStartApprovalPayload(
    bool enabled, std::uint64_t disabledAtFileTime = 0) noexcept
{
    std::array<std::uint8_t, kPortableAutoStartApprovalPayloadSize> payload{};
    payload[0] = enabled ? 0x02 : 0x03;
    if (!enabled)
    {
        for (std::size_t index = 0; index < sizeof(disabledAtFileTime);
             ++index)
        {
            payload[4 + index] = static_cast<std::uint8_t>(
                disabledAtFileTime >> (index * 8));
        }
    }
    return payload;
}

[[nodiscard]] constexpr bool IsPortableAutoStartApprovalActive(
    PortableAutoStartApprovalState state) noexcept
{
    return state == PortableAutoStartApprovalState::Missing ||
        state == PortableAutoStartApprovalState::Enabled;
}

[[nodiscard]] constexpr bool HasActivePortableAutoStart(
    PortableAutoStartRegistrationOwner owner,
    PortableAutoStartApprovalState approval) noexcept
{
    const bool registrationExists =
        owner == PortableAutoStartRegistrationOwner::CurrentExecutable ||
        owner == PortableAutoStartRegistrationOwner::OtherExecutable;
    return registrationExists &&
        IsPortableAutoStartApprovalActive(approval);
}

} // namespace snowdesktop
