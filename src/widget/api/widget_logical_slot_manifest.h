#pragma once

#include "common/json_value.h"
#include "widget/runtime/widget_logical_slot.h"

namespace snowdesktop::widget_runtime
{
bool ParseLogicalSlotDeclarations(const JsonValue* value,
    LogicalSlotDeclarations& declarations,
    std::vector<LogicalSlotManifestError>& errors);
}
