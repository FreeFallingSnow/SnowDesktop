#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace snowdesktop::steam_entitlement
{

constexpr std::chrono::hours kRegistrationLifetime =
    std::chrono::hours(24 * 7);

enum class State : std::uint8_t
{
    BridgeUnavailable,
    Unregistered,
    Checking,
    Registered,
    RegistrationFailed,
};

enum class Failure : std::uint8_t
{
    None,
    SteamUnavailable,
    NotOwned,
    BridgeError,
    StorageError,
};

struct Snapshot
{
    State state = State::BridgeUnavailable;
    Failure failure = Failure::None;
    bool bridgeAvailable = false;
    std::uint64_t revision = 0;
};

enum class BridgeOutcome : std::uint8_t
{
    Owned,
    NotOwned,
    SteamUnavailable,
    Failed,
};

struct BridgeResponse
{
    BridgeOutcome outcome = BridgeOutcome::Failed;
    std::uint64_t steamId = 0;
    std::string errorCode;
};

/** Parse the final JSON object returned by `entitlement status`. */
[[nodiscard]] BridgeResponse ParseBridgeResponse(
    std::string_view output, std::uint32_t exitCode);

/**
 * Application-lifetime Steam ownership registration service.
 *
 * Successful ownership checks are cached for seven days in a Windows
 * DPAPI-protected file. An absent or expired cache is unregistered and may be
 * checked once at startup or explicitly from Settings.
 */
class Service final
{
public:
    Service(std::filesystem::path bridgeExecutable,
        std::filesystem::path steamRuntime,
        std::filesystem::path protectedCache);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    [[nodiscard]] Snapshot Current() const noexcept;
    [[nodiscard]] bool IsRegistered() const noexcept;

    /** Start one asynchronous Bridge check when not already registered/busy. */
    [[nodiscard]] bool StartRegistration(std::function<void()> completed);
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::steam_entitlement
