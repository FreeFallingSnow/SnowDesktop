#pragma once

#include "../../steam_bridge/src/steam_connection_feedback.h"

#include <cstdint>
#include <string>

namespace snowdesktop::winui
{
enum class GeneralAdvancedFeatureState : std::uint8_t
{
    BridgeUnavailable,
    Unregistered,
    Checking,
    Registered,
    RegistrationFailed,
};

enum class GeneralAdvancedFeatureFailure : std::uint8_t
{
    None,
    SteamUnavailable,
    NotOwned,
    BridgeError,
    StorageError,
};

// Runtime status shared by the settings presenter and its private IPC codec.
struct GeneralAdvancedFeatureStatus
{
    GeneralAdvancedFeatureState state = GeneralAdvancedFeatureState::BridgeUnavailable;
    GeneralAdvancedFeatureFailure failure = GeneralAdvancedFeatureFailure::None;
    steam_bridge::SteamConnectionProblem connectionProblem = steam_bridge::SteamConnectionProblem::None;
    std::wstring errorDetail;
    bool bridgeAvailable = false;
    bool registered = false;
    std::int64_t validUntil = 0;
    bool cardVisible = false;
    bool offerSteamStore = false;
};
}
