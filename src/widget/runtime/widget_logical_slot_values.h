#pragma once

#include "widget/api/widget_logical_slot_declarations.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct LogicalSlotItem
{
    std::string id;
    std::string reference;
    std::string kind;
    std::string title;
    std::string source;
    std::string type;
    std::string target;
    bool available = true;

    bool operator==(const LogicalSlotItem&) const = default;
};

struct LogicalSlotSnapshot
{
    std::string id;
    LogicalSlotKind kind = LogicalSlotKind::Binding;
    std::uint64_t revision = 0;
    std::size_t capacity = 1;
    std::vector<LogicalSlotItem> items;
};

struct LogicalSlotChange
{
    std::string slotId;
    LogicalSlotKind kind = LogicalSlotKind::Binding;
    std::uint64_t revision = 0;
    std::string operation;
    std::vector<std::string> itemIds;
    std::string relatedSlotId;
    std::uint64_t relatedRevision = 0;
};
}
