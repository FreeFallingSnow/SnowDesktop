#pragma once

#include "widget_logical_slot_declarations.h"

#include <string>
#include <vector>

struct JsonValue;

namespace snowdesktop::widget_runtime
{
struct LogicalSlotManifestError
{
    std::string code;
    std::string message;
};

std::string SerializeLogicalSlotDeclarations(
    const LogicalSlotDeclarations& declarations);
bool ParseLogicalSlotDeclarations(const JsonValue* value,
    LogicalSlotDeclarations& declarations,
    std::vector<LogicalSlotManifestError>& errors);
}
