#pragma once
#include "edge_light_settings.h"
#include <string>

struct JsonValue;

namespace snowdesktop
{
bool DecodeEdgeLight(const JsonValue& input, EdgeLightSettings& output);
std::string EncodeEdgeLight(const EdgeLightSettings& value);
}
