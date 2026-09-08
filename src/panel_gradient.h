#pragma once

#include "json_value.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>
#include <vector>

namespace snowdesktop
{
// Host-owned full-panel gradient. The legacy bottom-bar gradientEndA has a
// different meaning and remains independent of this optional description.
struct PanelGradientStop
{
    double position = 0;
    std::uint32_t color = 0xb6d6ef;
    double opacity = .65;
    friend bool operator==(const PanelGradientStop&, const PanelGradientStop&) = default;
};
struct PanelGradient
{
    bool enabled = false;
    double angle = 90, start = 0, end = 1;
    std::vector<PanelGradientStop> stops{{0, 0xb6d6ef, .65}, {1, 0xd5c7ef, .65}};
    friend bool operator==(const PanelGradient&, const PanelGradient&) = default;
};
inline bool ValidatePanelGradient(const PanelGradient& gradient)
{
    const auto range = [](double v, double lo, double hi) {
        return std::isfinite(v) && v >= lo && v <= hi;
    };
    if (!range(gradient.angle, 0, 360) || !range(gradient.start, 0, 1) ||
        !range(gradient.end, 0, 1) || gradient.start >= gradient.end ||
        gradient.stops.size() < 2 || gradient.stops.size() > 5 ||
        gradient.stops.front().position != 0 || gradient.stops.back().position != 1)
        return false;
    double previous = -1;
    for (const auto& stop : gradient.stops)
    {
        if (!range(stop.position, 0, 1) || stop.position <= previous ||
            stop.color > 0xffffff || !range(stop.opacity, 0, 1)) return false;
        previous = stop.position;
    }
    return true;
}
inline bool DecodePanelGradient(const JsonValue& value, PanelGradient& output)
{
    if (!value.IsObject()) return false;
    PanelGradient result;
    const auto number = [](const JsonValue& object, const char* key, double& field) {
        const auto* input = object.Find(key);
        if (!input) return true;
        if (!input->IsNumber() || !std::isfinite(input->number)) return false;
        field = input->number;
        return true;
    };
    if (const auto* enabled = value.Find("enabled"))
    {
        if (!enabled->IsBoolean()) return false;
        result.enabled = enabled->boolean;
    }
    if (!number(value, "angle", result.angle) || !number(value, "start", result.start) ||
        !number(value, "end", result.end)) return false;
    if (const auto* stops = value.Find("stops"))
    {
        if (!stops->IsArray() || stops->array.size() < 2 || stops->array.size() > 5) return false;
        result.stops.clear();
        for (const auto& item : stops->array)
        {
            PanelGradientStop stop;
            double color = 0;
            if (!item.IsObject() || !item.Find("position") || !item.Find("color") ||
                !item.Find("opacity") || !number(item, "position", stop.position) ||
                !number(item, "color", color) || !number(item, "opacity", stop.opacity) ||
                color < 0 || color > 0xffffff || std::floor(color) != color) return false;
            stop.color = static_cast<std::uint32_t>(color);
            result.stops.push_back(stop);
        }
    }
    if (!ValidatePanelGradient(result)) return false;
    output = std::move(result);
    return true;
}
inline std::string EncodePanelGradient(const PanelGradient& gradient)
{
    if (!ValidatePanelGradient(gradient)) return {};
    std::ostringstream output;
    output.imbue(std::locale::classic()); output.precision(17);
    output << "{\"enabled\":" << (gradient.enabled ? "true" : "false") <<
        ",\"angle\":" << gradient.angle << ",\"start\":" << gradient.start <<
        ",\"end\":" << gradient.end << ",\"stops\":[";
    bool first = true;
    for (const auto& stop : gradient.stops)
    {
        if (!first) output << ',';
        first = false;
        output << "{\"position\":" << stop.position << ",\"color\":" << stop.color <<
            ",\"opacity\":" << stop.opacity << '}';
    }
    output << "]}";
    return output.str();
}

struct PanelGradientLine { double x1, y1, x2, y2; };
inline PanelGradientLine ResolvePanelGradientLine(const PanelGradient& gradient, double width, double height)
{
    const double angle = gradient.angle * 3.14159265358979323846 / 180;
    const double dx = std::cos(angle), dy = std::sin(angle);
    // Project the complete rectangle onto the direction, including diagonal
    // corners. A percentage range always means the same thing after resizing.
    const double length = std::abs(dx) * width + std::abs(dy) * height;
    return {width / 2 + dx * length * (gradient.start - .5),
        height / 2 + dy * length * (gradient.start - .5),
        width / 2 + dx * length * (gradient.end - .5),
        height / 2 + dy * length * (gradient.end - .5)};
}
}
