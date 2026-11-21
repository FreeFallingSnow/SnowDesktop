#pragma once

#include "widget/view/widget_interaction_types.h"

#include <cstddef>
#include <string>
#include <unordered_set>

struct lua_State;

namespace snowdesktop::widget_runtime
{
bool ReadInteractionValue(lua_State* state, int index, InteractionValue& output,
    std::size_t depth, std::size_t& nodes, std::size_t& stringBytes,
    std::unordered_set<const void*>& ancestors, std::string& error);
void PushInteractionValue(lua_State* state, const InteractionValue& value);
}
