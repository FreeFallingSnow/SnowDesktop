#pragma once
#include "settings/settings_ipc_values.h"
#include "platform/window_support/window_capabilities.h"

namespace snowdesktop::settings_ipc
{
template<> struct Fields<window_support::CapabilitySnapshot>
{
    template<class Value> static auto Tie(Value& v)
    { return std::tie(v.inProcess, v.helper, v.helperState, v.generation); }
};
template<> struct Fields<window_support::SupportStatus>
{
    template<class Value> static auto Tie(Value& v)
    { return std::tie(v.capabilities, v.authorization, v.hostElevated, v.error, v.updateAvailable); }
};
}
