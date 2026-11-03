#pragma once

#include "dock_tray_types.h"
#include <iosfwd>

struct JsonValue;

namespace snowdesktop::dock_tray
{
bool DecodeRules(const JsonValue& document, std::vector<DockTrayRule>& rules);

bool WriteRules(std::ostream& output, const std::vector<DockTrayRule>& rules);
}
