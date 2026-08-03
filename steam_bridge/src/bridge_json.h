// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::steam_bridge
{
struct JsonValue
{
    enum class Type { Null, Boolean, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    static JsonValue Boolean(bool value)
    {
        JsonValue result;
        result.type = Type::Boolean;
        result.boolean = value;
        return result;
    }
    static JsonValue Number(double value)
    {
        JsonValue result;
        result.type = Type::Number;
        result.number = value;
        return result;
    }
    static JsonValue String(std::string value)
    {
        JsonValue result;
        result.type = Type::String;
        result.string = std::move(value);
        return result;
    }
    static JsonValue Array()
    {
        JsonValue result;
        result.type = Type::Array;
        return result;
    }
    static JsonValue Object()
    {
        JsonValue result;
        result.type = Type::Object;
        return result;
    }

    bool IsNull() const { return type == Type::Null; }
    bool IsBoolean() const { return type == Type::Boolean; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const JsonValue* Find(std::string_view key) const
    {
        if (!IsObject()) return nullptr;
        const auto found = object.find(std::string(key));
        return found == object.end() ? nullptr : &found->second;
    }
};

namespace json_detail
{
inline void AppendUtf8(std::string& output, std::uint32_t codePoint)
{
    if (codePoint <= 0x7f)
        output.push_back(static_cast<char>(codePoint));
    else if (codePoint <= 0x7ff)
    {
        output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
    else if (codePoint <= 0xffff)
    {
        output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
    else
    {
        output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

class Parser
{
public:
    explicit Parser(std::string_view input) : input_(input) {}

    bool Parse(JsonValue& value, std::string& error)
    {
        SkipSpace();
        if (!ReadValue(value))
        {
            error = error_.empty() ? "invalid JSON" : error_;
            return false;
        }
        SkipSpace();
        if (position_ != input_.size())
        {
            error = "unexpected trailing JSON content";
            return false;
        }
        return true;
    }

private:
    bool Fail(std::string message)
    {
        if (error_.empty())
            error_ = std::move(message) + " at byte " +
                std::to_string(position_);
        return false;
    }

    void SkipSpace()
    {
        while (position_ < input_.size() &&
            (input_[position_] == ' ' || input_[position_] == '\t' ||
             input_[position_] == '\r' || input_[position_] == '\n'))
            ++position_;
    }

    bool Consume(std::string_view token)
    {
        if (input_.substr(position_, token.size()) != token) return false;
        position_ += token.size();
        return true;
    }

    bool ReadValue(JsonValue& value)
    {
        SkipSpace();
        if (position_ >= input_.size()) return Fail("expected a value");
        switch (input_[position_])
        {
        case 'n':
            if (!Consume("null")) return Fail("invalid null literal");
            value = {};
            return true;
        case 't':
            if (!Consume("true")) return Fail("invalid true literal");
            value = JsonValue::Boolean(true);
            return true;
        case 'f':
            if (!Consume("false")) return Fail("invalid false literal");
            value = JsonValue::Boolean(false);
            return true;
        case '"':
        {
            std::string text;
            if (!ReadString(text)) return false;
            value = JsonValue::String(std::move(text));
            return true;
        }
        case '[': return ReadArray(value);
        case '{': return ReadObject(value);
        default: return ReadNumber(value);
        }
    }

    bool ReadHex4(std::uint32_t& value)
    {
        value = 0;
        for (int index = 0; index < 4; ++index)
        {
            if (position_ >= input_.size()) return false;
            const char character = input_[position_++];
            unsigned digit = 0;
            if (character >= '0' && character <= '9')
                digit = static_cast<unsigned>(character - '0');
            else if (character >= 'a' && character <= 'f')
                digit = static_cast<unsigned>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F')
                digit = static_cast<unsigned>(character - 'A' + 10);
            else return false;
            value = value * 16 + digit;
        }
        return true;
    }

    bool ReadString(std::string& output)
    {
        if (input_[position_++] != '"') return false;
        output.clear();
        while (position_ < input_.size())
        {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20) return Fail("control character in string");
            if (character != '\\')
            {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) return Fail("unfinished escape");
            const char escape = input_[position_++];
            switch (escape)
            {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u':
            {
                std::uint32_t first = 0;
                if (!ReadHex4(first)) return Fail("invalid unicode escape");
                if (first >= 0xd800 && first <= 0xdbff)
                {
                    if (!Consume("\\u")) return Fail("missing low surrogate");
                    std::uint32_t second = 0;
                    if (!ReadHex4(second) || second < 0xdc00 ||
                        second > 0xdfff)
                        return Fail("invalid low surrogate");
                    first = 0x10000 + ((first - 0xd800) << 10) +
                        (second - 0xdc00);
                }
                else if (first >= 0xdc00 && first <= 0xdfff)
                    return Fail("unexpected low surrogate");
                AppendUtf8(output, first);
                break;
            }
            default: return Fail("unknown string escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ReadNumber(JsonValue& value)
    {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return Fail("invalid number");
        if (input_[position_] == '0') ++position_;
        else
        {
            if (input_[position_] < '1' || input_[position_] > '9')
                return Fail("invalid number");
            while (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.')
        {
            ++position_;
            const std::size_t decimal = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') ++position_;
            if (decimal == position_) return Fail("invalid decimal");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E'))
        {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-'))
                ++position_;
            const std::size_t exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') ++position_;
            if (exponent == position_) return Fail("invalid exponent");
        }
        const std::string token(input_.substr(begin, position_ - begin));
        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(parsed))
            return Fail("number is out of range");
        value = JsonValue::Number(parsed);
        return true;
    }

    bool ReadArray(JsonValue& value)
    {
        ++position_;
        value = JsonValue::Array();
        SkipSpace();
        if (position_ < input_.size() && input_[position_] == ']')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            JsonValue entry;
            if (!ReadValue(entry)) return false;
            value.array.push_back(std::move(entry));
            SkipSpace();
            if (position_ >= input_.size()) return Fail("unterminated array");
            if (input_[position_] == ']')
            {
                ++position_;
                return true;
            }
            if (input_[position_++] != ',') return Fail("expected comma");
        }
    }

    bool ReadObject(JsonValue& value)
    {
        ++position_;
        value = JsonValue::Object();
        SkipSpace();
        if (position_ < input_.size() && input_[position_] == '}')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            SkipSpace();
            if (position_ >= input_.size() || input_[position_] != '"')
                return Fail("expected object key");
            std::string key;
            if (!ReadString(key)) return false;
            SkipSpace();
            if (position_ >= input_.size() || input_[position_++] != ':')
                return Fail("expected colon");
            JsonValue entry;
            if (!ReadValue(entry)) return false;
            if (!value.object.emplace(std::move(key), std::move(entry)).second)
                return Fail("duplicate object key");
            SkipSpace();
            if (position_ >= input_.size()) return Fail("unterminated object");
            if (input_[position_] == '}')
            {
                ++position_;
                return true;
            }
            if (input_[position_++] != ',') return Fail("expected comma");
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
    std::string error_;
};

inline void EscapeString(std::string_view value, std::string& output)
{
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20)
            {
                output += "\\u00";
                output.push_back(hex[character >> 4]);
                output.push_back(hex[character & 0x0f]);
            }
            else output.push_back(static_cast<char>(character));
            break;
        }
    }
    output.push_back('"');
}

inline void WriteValue(const JsonValue& value, std::string& output,
    int indent, int depth)
{
    auto newline = [&]
    {
        if (indent < 0) return;
        output.push_back('\n');
        output.append(static_cast<std::size_t>((depth + 1) * indent), ' ');
    };
    switch (value.type)
    {
    case JsonValue::Type::Null: output += "null"; break;
    case JsonValue::Type::Boolean: output += value.boolean ? "true" : "false"; break;
    case JsonValue::Type::Number:
    {
        char buffer[64]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer),
            value.number);
        output.append(buffer, result.ptr);
        break;
    }
    case JsonValue::Type::String: EscapeString(value.string, output); break;
    case JsonValue::Type::Array:
        output.push_back('[');
        for (std::size_t index = 0; index < value.array.size(); ++index)
        {
            if (index) output.push_back(',');
            newline();
            WriteValue(value.array[index], output, indent, depth + 1);
        }
        if (!value.array.empty() && indent >= 0)
        {
            output.push_back('\n');
            output.append(static_cast<std::size_t>(depth * indent), ' ');
        }
        output.push_back(']');
        break;
    case JsonValue::Type::Object:
        output.push_back('{');
        {
            std::size_t index = 0;
            for (const auto& [key, entry] : value.object)
            {
                if (index++) output.push_back(',');
                newline();
                EscapeString(key, output);
                output += indent < 0 ? ":" : ": ";
                WriteValue(entry, output, indent, depth + 1);
            }
        }
        if (!value.object.empty() && indent >= 0)
        {
            output.push_back('\n');
            output.append(static_cast<std::size_t>(depth * indent), ' ');
        }
        output.push_back('}');
        break;
    }
}
}

inline bool ParseJson(std::string_view input, JsonValue& value,
    std::string& error)
{
    return json_detail::Parser(input).Parse(value, error);
}

inline std::string WriteJson(const JsonValue& value, int indent = 2)
{
    std::string output;
    json_detail::WriteValue(value, output, indent, 0);
    return output;
}

inline std::optional<std::string> JsonString(const JsonValue& object,
    std::string_view key)
{
    const JsonValue* value = object.Find(key);
    return value && value->IsString()
        ? std::optional<std::string>(value->string) : std::nullopt;
}

inline std::optional<bool> JsonBoolean(const JsonValue& object,
    std::string_view key)
{
    const JsonValue* value = object.Find(key);
    return value && value->IsBoolean()
        ? std::optional<bool>(value->boolean) : std::nullopt;
}

inline std::optional<std::uint64_t> JsonUnsigned(const JsonValue& object,
    std::string_view key)
{
    const JsonValue* value = object.Find(key);
    if (!value || !value->IsNumber() || !std::isfinite(value->number) ||
        value->number < 0 || std::floor(value->number) != value->number ||
        value->number > 9007199254740991.0)
        return std::nullopt;
    return static_cast<std::uint64_t>(value->number);
}
}
