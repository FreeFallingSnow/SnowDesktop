#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct InteractionValue
{
    enum class Type
    {
        Null,
        Boolean,
        Integer,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean = false;
    long long integer = 0;
    double number = 0.0;
    std::string string;
    std::vector<InteractionValue> array;
    std::map<std::string, InteractionValue, std::less<>> object;

    bool operator==(const InteractionValue&) const = default;
};

struct InteractionAction
{
    enum class ContextMenuScope
    {
        Element,
        Component,
    };

    std::string id;
    InteractionValue value;
    ContextMenuScope contextMenuScope = ContextMenuScope::Element;

    bool operator==(const InteractionAction&) const = default;
};
}
