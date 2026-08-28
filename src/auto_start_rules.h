#pragma once

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
