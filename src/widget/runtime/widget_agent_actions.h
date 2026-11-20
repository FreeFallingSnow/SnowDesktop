#pragma once

#include "common/json_value.h"
#include "widget/view/widget_interaction_types.h"
#include <cmath>
#include <map>
#include <string>
#include <string_view>

namespace snowdesktop::widget_runtime
{
struct AgentArgument
{
    std::string type;
    std::string description;
    bool required = true;
};

struct AgentAction
{
    std::string description;
    std::map<std::string, AgentArgument, std::less<>> arguments;
};

using AgentActions = std::map<std::string, AgentAction, std::less<>>;

inline bool IsAgentActionIdentifier(std::string_view key)
{
    if (key.empty() || key.size() > 64)
        return false;
    for (const char character : key)
        if (!((character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-' || character == '.'))
            return false;
    return true;
}

inline bool ReadAgentActions(const InteractionValue& definition, AgentActions& output)
{
    using Type = InteractionValue::Type;
    if (definition.type == Type::Null)
        return true;
    if (definition.type != Type::Object || definition.object.size() > 16)
        return false;
    for (const auto& [id, value] : definition.object)
    {
        if (!IsAgentActionIdentifier(id) || value.type != Type::Object)
            return false;
        const auto description = value.object.find("description");
        const auto arguments = value.object.find("arguments");
        if (description == value.object.end() || description->second.type != Type::String ||
            description->second.string.empty() || description->second.string.size() > 1024 ||
            value.object.size() != (arguments == value.object.end() ? 1u : 2u))
            return false;
        AgentAction action;
        action.description = description->second.string;
        if (arguments != value.object.end())
        {
            if (arguments->second.type != Type::Object || arguments->second.object.size() > 16)
                return false;
            for (const auto& [name, parameter] : arguments->second.object)
            {
                if (!IsAgentActionIdentifier(name) || parameter.type != Type::Object)
                    return false;
                const auto type = parameter.object.find("type");
                const auto help = parameter.object.find("description");
                const auto required = parameter.object.find("required");
                if (type == parameter.object.end() || type->second.type != Type::String ||
                    help == parameter.object.end() || help->second.type != Type::String ||
                    help->second.string.empty() || help->second.string.size() > 1024 ||
                    (required != parameter.object.end() && required->second.type != Type::Boolean) ||
                    parameter.object.size() != (required == parameter.object.end() ? 2u : 3u))
                    return false;
                const std::string& nameOfType = type->second.string;
                if (nameOfType != "boolean" && nameOfType != "string" &&
                    nameOfType != "number" && nameOfType != "integer")
                    return false;
                action.arguments.emplace(name, AgentArgument{nameOfType, help->second.string,
                    required == parameter.object.end() || required->second.boolean});
            }
        }
        output.emplace(id, std::move(action));
    }
    return true;
}

inline bool ValidateAgentArguments(const AgentAction& action, const JsonValue& arguments)
{
    if (!arguments.IsObject() || arguments.object.size() > action.arguments.size())
        return false;
    std::size_t matched = 0;
    for (const auto& [name, field] : action.arguments)
    {
        const auto* value = arguments.Find(name);
        if (!value)
        {
            if (field.required)
                return false;
            continue;
        }
        ++matched;
        if (field.type == "string" && (!value->IsString() || value->string.size() > 4096))
            return false;
        if (field.type == "boolean" && !value->IsBoolean())
            return false;
        if ((field.type == "number" || field.type == "integer") &&
            (!value->IsNumber() || !std::isfinite(value->number)))
            return false;
        if (field.type == "integer" &&
            (std::abs(value->number) > 9007199254740991.0 || std::floor(value->number) != value->number))
            return false;
    }
    return matched == arguments.object.size();
}
}
