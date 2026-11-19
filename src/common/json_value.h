#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct JsonValue
{
    enum class Type
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::unordered_map<std::string, JsonValue> object;

    bool IsNull() const { return type == Type::Null; }
    bool IsBoolean() const { return type == Type::Boolean; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const JsonValue* Find(std::string_view key) const
    {
        if (!IsObject())
            return nullptr;
        auto found = object.find(std::string(key));
        return found == object.end() ? nullptr : &found->second;
    }
};

bool ParseJson(std::string_view text, JsonValue& output,
    std::string* error = nullptr);
