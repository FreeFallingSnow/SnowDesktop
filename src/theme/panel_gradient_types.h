#pragma once
#include <cstdint>
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
struct PanelGradientLine { double x1, y1, x2, y2; };
}
