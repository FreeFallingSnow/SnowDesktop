#pragma once

#include "dock_fullscreen_types.h"
#include <vector>

struct JsonValue;

namespace snowdesktop::dock_fullscreen
{
DockFullscreenPolicy DecodePolicy(const JsonValue& document);

bool DecodeExceptions(const JsonValue& document, std::vector<DockFullscreenException>& entries);
}
