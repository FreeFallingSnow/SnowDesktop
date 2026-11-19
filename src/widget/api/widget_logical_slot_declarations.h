#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class LogicalSlotKind
{
    Binding,
    Collection,
};

struct LogicalSlotDeclaration
{
    LogicalSlotKind kind = LogicalSlotKind::Binding;
    std::vector<std::string> accepts;
    std::string operation = "reference";
    std::string replacePolicy = "allow";
    bool allowClear = true;
    std::size_t capacity = 1;

    bool operator==(const LogicalSlotDeclaration&) const = default;
};

using LogicalSlotDeclarations =
    std::map<std::string, LogicalSlotDeclaration, std::less<>>;

const char* LogicalSlotKindName(LogicalSlotKind kind) noexcept;
}
