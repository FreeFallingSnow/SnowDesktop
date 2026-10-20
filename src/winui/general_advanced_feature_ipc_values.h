#pragma once

#include "settings/settings_ipc_codec.h"
#include "general_advanced_feature_status.h"

namespace snowdesktop::settings_ipc
{
template<> struct Fields<winui::GeneralAdvancedFeatureStatus>
{
    template<class Value> static auto Tie(Value& value)
    {
        return std::tie(value.state, value.failure, value.bridgeAvailable, value.registered,
            value.validUntil, value.cardVisible, value.offerSteamStore,
            value.connectionProblem, value.errorDetail);
    }
};
}
