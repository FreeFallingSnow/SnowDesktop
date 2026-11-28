#pragma once

#include "common/json_value.h"
#include "dock_layout_settings.h"
#include <cmath>
#include <limits>
#include <ostream>
#include <type_traits>

namespace snowdesktop
{
template <typename Override>
bool ReadBarMonitorOverride(const JsonValue& root, const char* key, Override& result)
{
    const auto* object = root.Find(key);
    if (!object) return true;
    if (!object->IsObject()) return false;
    bool valid = true;
    Override::Visit(result, [&](const char* name, auto& value) {
        const auto* field = object->Find(name);
        if (!field || field->IsNull()) return;
        using Value = typename std::remove_reference_t<decltype(value)>::value_type;
        if constexpr (std::is_same_v<Value, bool>)
        {
            if (field->IsBoolean()) value = field->boolean;
            else valid = false;
        }
        else if constexpr (std::is_same_v<Value, float>)
        {
            if (field->IsNumber() && std::isfinite(field->number) &&
                std::abs(field->number) <= (std::numeric_limits<float>::max)())
                value = static_cast<float>(field->number);
            else valid = false;
        }
        else
        {
            constexpr int maximum = std::is_same_v<Value, DockPosition> ? 3 : 1;
            if (field->IsNumber() && field->number >= 0 && field->number <= maximum &&
                std::floor(field->number) == field->number)
                value = static_cast<Value>(static_cast<int>(field->number));
            else valid = false;
        }
    });
    return valid;
}

template <typename Override>
void WriteBarMonitorOverride(std::ostream& text, const Override& value)
{
    text << '{';
    bool first = true;
    Override::Visit(value, [&](const char* key, const auto& field) {
        if (!field) return;
        if (!first) text << ',';
        first = false;
        text << '"' << key << "\":";
        using Value = typename std::remove_cvref_t<decltype(field)>::value_type;
        if constexpr (std::is_same_v<Value, bool>) text << (*field ? "true" : "false");
        else if constexpr (std::is_enum_v<Value>) text << static_cast<int>(*field);
        else text << *field;
    });
    text << '}';
}
}
