#pragma once

#include "json_value.h"
#include "widget_logical_slot.h"

namespace snowdesktop::widget_runtime
{
bool ParseLogicalSlotDeclarations(const JsonValue* value,
    LogicalSlotDeclarations& declarations,
    std::vector<LogicalSlotManifestError>& errors);
}
