#include "edge_light_settings.h"
#include <algorithm>
#include <cmath>

namespace snowdesktop
{
EdgeLightSettings NormalizeEdgeLight(EdgeLightSettings value)
{
    const EdgeLightSettings defaults;
    VisitEdgeLightFields([&](auto, auto field, float minimum, float maximum) {
        value.*field = std::isfinite(value.*field)
            ? std::clamp(value.*field, minimum, maximum) : defaults.*field;
    });
    return value;
}

bool ValidateEdgeLight(const EdgeLightSettings& value)
{
    bool valid = true;
    VisitEdgeLightFields([&](auto, auto field, float minimum, float maximum) {
        valid = valid && std::isfinite(value.*field) && value.*field >= minimum && value.*field <= maximum;
    });
    return valid;
}
}
