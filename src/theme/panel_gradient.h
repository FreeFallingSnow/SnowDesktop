#pragma once
#include "panel_gradient_types.h"
#include <string>

struct JsonValue;

namespace snowdesktop
{
bool ValidatePanelGradient(const PanelGradient& gradient);
bool DecodePanelGradient(const JsonValue& value, PanelGradient& output);
std::string EncodePanelGradient(const PanelGradient& gradient);

PanelGradientLine ResolvePanelGradientLine(const PanelGradient& gradient, double width, double height);
}
