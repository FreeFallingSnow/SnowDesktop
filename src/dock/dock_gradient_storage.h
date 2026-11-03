#pragma once

#include <iosfwd>

struct JsonValue;
struct DockSettings;

namespace snowdesktop
{
bool ReadTaskbarGradients(const JsonValue& document, DockSettings& settings);

bool WriteTaskbarGradients(std::ostream& output, const DockSettings& settings);
}
