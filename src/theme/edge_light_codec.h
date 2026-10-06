#pragma once
#include "edge_light_settings.h"
#include "common/json_value.h"
#include <locale>
#include <sstream>

namespace snowdesktop
{
inline bool DecodeEdgeLight(const JsonValue& input, EdgeLightSettings& output)
{
    if (!input.IsObject()) return false;
    EdgeLightSettings value;
    bool valid = true;
    VisitEdgeLightFields([&](auto key, auto field, float minimum, float maximum) {
        if (const auto* number = input.Find(key))
        {
            const float decoded = static_cast<float>(number->number);
            if (!number->IsNumber() || !std::isfinite(number->number) || !std::isfinite(decoded) || decoded < minimum || decoded > maximum) valid = false;
            else value.*field = decoded;
        }
    });
    if (valid) output = value;
    return valid;
}
inline std::string EncodeEdgeLight(const EdgeLightSettings& value)
{
    if (!ValidateEdgeLight(value)) return {};
    std::ostringstream output; output.imbue(std::locale::classic()); output.precision(9);
    output << '{'; bool first = true;
    VisitEdgeLightFields([&](auto key, auto field, float, float) {
        if (!first) output << ',';
        first = false;
        output << '"' << key << "\":" << value.*field;
    });
    output << '}'; return output.str();
}
}
