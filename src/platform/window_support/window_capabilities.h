#pragma once

#include <cstdint>

namespace snowdesktop::window_support
{
// Private host/helper contract. Consumers select an operation, never a token
// elevation level. Providers publish only capabilities they have established.
enum class Capability : std::uint32_t
{
    None = 0,
    PointerObservation = 1u << 0,
    WindowPin = 1u << 1,
    WindowControl = 1u << 2,
    MinimizeAnimation = 1u << 3,
};
using CapabilityMask = std::uint32_t;
constexpr CapabilityMask Mask(Capability capability) noexcept
{ return static_cast<CapabilityMask>(capability); }
constexpr bool Contains(CapabilityMask available, Capability capability) noexcept
{ return capability != Capability::None && (available & Mask(capability)) == Mask(capability); }

enum class Provider : std::uint8_t { Unavailable, InProcess, Helper };
enum class ProviderState : std::uint8_t { Absent, Starting, Ready, Failed };
enum class UnavailableReason : std::uint8_t
{ None, Unsupported, Starting, Disconnected, TargetUnavailable };

struct CapabilitySnapshot
{
    CapabilityMask inProcess = 0;
    CapabilityMask helper = 0;
    ProviderState helperState = ProviderState::Absent;
    std::uint64_t generation = 0;
};

struct OperationContext
{
    // Target lifetime, session and provider-specific access checks belong to
    // the provider. A valid global connection is not access to every HWND.
    bool targetCurrent = true;
    bool inProcessEligible = true;
    bool helperEligible = true;
};

struct CapabilityRoute
{
    Provider provider = Provider::Unavailable;
    UnavailableReason reason = UnavailableReason::Unsupported;
    std::uint64_t generation = 0;
    constexpr bool Available() const noexcept { return provider != Provider::Unavailable; }
    constexpr bool NeedsFallback() const noexcept { return !Available(); }
};

constexpr CapabilityRoute ResolveCapability(const CapabilitySnapshot& snapshot,
    Capability capability, OperationContext context = {}) noexcept
{
    if (!context.targetCurrent)
        return {Provider::Unavailable, UnavailableReason::TargetUnavailable, snapshot.generation};
    if (context.inProcessEligible && Contains(snapshot.inProcess, capability))
        return {Provider::InProcess, UnavailableReason::None, snapshot.generation};
    if (context.helperEligible && snapshot.helperState == ProviderState::Ready &&
        Contains(snapshot.helper, capability))
        return {Provider::Helper, UnavailableReason::None, snapshot.generation};
    const auto reason = snapshot.helperState == ProviderState::Starting
        ? UnavailableReason::Starting : snapshot.helperState == ProviderState::Failed
        ? UnavailableReason::Disconnected : UnavailableReason::Unsupported;
    return {Provider::Unavailable, reason, snapshot.generation};
}

enum class AuthorizationState : std::uint8_t { NotRegistered, Registered, Invalid, Configuring, AwaitingConsent };
constexpr bool AuthorizationPending(AuthorizationState state) noexcept
{ return state == AuthorizationState::Configuring || state == AuthorizationState::AwaitingConsent; }
struct SupportStatus
{
    CapabilitySnapshot capabilities;
    AuthorizationState authorization = AuthorizationState::NotRegistered;
    // Informational only, for the settings card and avoiding an unnecessary
    // second privileged process. This field never selects feature fallbacks.
    bool hostElevated = false;
    std::uint32_t error = 0;
};
}
