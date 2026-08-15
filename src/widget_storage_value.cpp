#include "widget_storage_value.h"

#include "json_value.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
bool IsValidUtf8(std::string_view value) noexcept
{
    std::size_t index = 0;
    while (index < value.size())
    {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        if (lead <= 0x7f)
        {
            length = 1;
            codePoint = lead;
        }
        else if (lead >= 0xc2 && lead <= 0xdf)
        {
            length = 2;
            codePoint = lead & 0x1f;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            length = 3;
            codePoint = lead & 0x0f;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            length = 4;
            codePoint = lead & 0x07;
        }
        else
            return false;
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && codePoint < 0x80) ||
            (length == 3 && codePoint < 0x800) ||
            (length == 4 && codePoint < 0x10000) ||
            codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return false;
        index += length;
    }
    return true;
}

void AppendJsonString(std::string_view value, std::string& output)
{
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char byte : value)
    {
        switch (byte)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20)
            {
                output += "\\u00";
                output.push_back(hex[byte >> 4]);
                output.push_back(hex[byte & 0x0f]);
            }
            else
                output.push_back(static_cast<char>(byte));
            break;
        }
    }
    output.push_back('"');
}

bool AppendValue(const InteractionValue& value, std::string& output,
    std::size_t depth, std::size_t& nodes, std::size_t& stringBytes,
    std::string& error)
{
    using Type = InteractionValue::Type;
    if (++nodes > MaximumTypedStorageNodes ||
        depth > MaximumTypedStorageDepth)
    {
        error = "value exceeds the typed storage node or depth limit";
        return false;
    }
    const auto appendString = [&](std::string_view text) {
        if (!IsValidUtf8(text) ||
            text.size() > MaximumTypedStorageStringBytes ||
            stringBytes > MaximumTypedStorageStringBytes - text.size())
        {
            error = "typed storage strings exceed 16 KiB or contain invalid UTF-8";
            return false;
        }
        stringBytes += text.size();
        AppendJsonString(text, output);
        return true;
    };

    switch (value.type)
    {
    case Type::Null:
        output += "[\"z\"]";
        break;
    case Type::Boolean:
        output += value.boolean ? "[\"b\",true]" : "[\"b\",false]";
        break;
    case Type::Integer:
        output += "[\"i\",\"";
        output += std::to_string(value.integer);
        output += "\"]";
        break;
    case Type::Number:
    {
        if (!std::isfinite(value.number))
        {
            error = "typed storage numbers must be finite";
            return false;
        }
        char buffer[64]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer),
            value.number, std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (result.ec != std::errc{})
        {
            error = "typed storage number encoding failed";
            return false;
        }
        output += "[\"n\",";
        output.append(buffer, result.ptr);
        output.push_back(']');
        break;
    }
    case Type::String:
        output += "[\"s\",";
        if (!appendString(value.string)) return false;
        output.push_back(']');
        break;
    case Type::Array:
        output += "[\"a\",[";
        for (std::size_t index = 0; index < value.array.size(); ++index)
        {
            if (index != 0) output.push_back(',');
            if (!AppendValue(value.array[index], output, depth + 1,
                    nodes, stringBytes, error))
                return false;
        }
        output += "]]";
        break;
    case Type::Object:
    {
        output += "[\"o\",{";
        bool first = true;
        for (const auto& [key, child] : value.object)
        {
            if (!first) output.push_back(',');
            first = false;
            if (key.empty() || key.size() > 128 || !appendString(key))
            {
                if (error.empty())
                    error = "typed storage object keys must contain 1 to 128 UTF-8 bytes";
                return false;
            }
            output.push_back(':');
            if (!AppendValue(child, output, depth + 1,
                    nodes, stringBytes, error))
                return false;
        }
        output += "}]";
        break;
    }
    }
    if (output.size() > MaximumTypedStorageEncodedBytes)
    {
        error = "encoded typed storage value exceeds 64 KiB";
        return false;
    }
    return true;
}

bool JsonNestingIsBounded(std::string_view value) noexcept
{
    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char ch : value)
    {
        if (inString)
        {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                inString = false;
            continue;
        }
        if (ch == '"')
            inString = true;
        else if (ch == '[' || ch == '{')
        {
            if (++depth > 32) return false;
        }
        else if ((ch == ']' || ch == '}') && depth > 0)
            --depth;
    }
    return true;
}

bool ReadValue(const JsonValue& encoded, InteractionValue& output,
    std::size_t depth, std::size_t& nodes, std::size_t& stringBytes,
    std::string& error)
{
    using Type = InteractionValue::Type;
    if (++nodes > MaximumTypedStorageNodes ||
        depth > MaximumTypedStorageDepth || !encoded.IsArray() ||
        encoded.array.empty() || !encoded.array[0].IsString())
    {
        error = "typed storage value has an invalid shape or exceeds its limits";
        return false;
    }
    const std::string& tag = encoded.array[0].string;
    const auto readString = [&](const JsonValue& value,
                                std::string& destination) {
        if (!value.IsString() || !IsValidUtf8(value.string) ||
            value.string.size() > MaximumTypedStorageStringBytes ||
            stringBytes > MaximumTypedStorageStringBytes - value.string.size())
        {
            error = "typed storage strings exceed 16 KiB or contain invalid UTF-8";
            return false;
        }
        stringBytes += value.string.size();
        destination = value.string;
        return true;
    };

    if (tag == "z" && encoded.array.size() == 1)
    {
        output = {};
        output.type = Type::Null;
        return true;
    }
    if (tag == "b" && encoded.array.size() == 2 &&
        encoded.array[1].IsBoolean())
    {
        output = {};
        output.type = Type::Boolean;
        output.boolean = encoded.array[1].boolean;
        return true;
    }
    if (tag == "i" && encoded.array.size() == 2 &&
        encoded.array[1].IsString())
    {
        long long integer = 0;
        const std::string& text = encoded.array[1].string;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), integer);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size())
        {
            error = "typed storage integer is invalid";
            return false;
        }
        output = {};
        output.type = Type::Integer;
        output.integer = integer;
        return true;
    }
    if (tag == "n" && encoded.array.size() == 2 &&
        encoded.array[1].IsNumber() &&
        std::isfinite(encoded.array[1].number))
    {
        output = {};
        output.type = Type::Number;
        output.number = encoded.array[1].number;
        return true;
    }
    if (tag == "s" && encoded.array.size() == 2)
    {
        output = {};
        output.type = Type::String;
        return readString(encoded.array[1], output.string);
    }
    if (tag == "a" && encoded.array.size() == 2 &&
        encoded.array[1].IsArray())
    {
        if (encoded.array[1].array.size() > MaximumTypedStorageNodes - nodes)
        {
            error = "typed storage array exceeds the node limit";
            return false;
        }
        output = {};
        output.type = Type::Array;
        output.array.resize(encoded.array[1].array.size());
        for (std::size_t index = 0;
             index < encoded.array[1].array.size(); ++index)
        {
            if (!ReadValue(encoded.array[1].array[index],
                    output.array[index], depth + 1,
                    nodes, stringBytes, error))
                return false;
        }
        return true;
    }
    if (tag == "o" && encoded.array.size() == 2 &&
        encoded.array[1].IsObject())
    {
        if (encoded.array[1].object.size() > MaximumTypedStorageNodes - nodes)
        {
            error = "typed storage object exceeds the node limit";
            return false;
        }
        output = {};
        output.type = Type::Object;
        for (const auto& [key, child] : encoded.array[1].object)
        {
            if (key.empty() || key.size() > 128 || !IsValidUtf8(key) ||
                stringBytes > MaximumTypedStorageStringBytes - key.size())
            {
                error = "typed storage object key is invalid or exceeds the string budget";
                return false;
            }
            stringBytes += key.size();
            InteractionValue decoded;
            if (!ReadValue(child, decoded, depth + 1,
                    nodes, stringBytes, error))
                return false;
            output.object.emplace(key,
                std::move(decoded));
        }
        return true;
    }

    error = "typed storage value uses an unknown or malformed tag";
    return false;
}
}

std::string TypedStorageMetadataKey(std::string_view key)
{
    std::string result(TypedStorageMetadataPrefix);
    result.append(key);
    return result;
}

bool EncodeTypedStorageValue(const InteractionValue& value,
    std::string& output, std::string& error)
{
    output.clear();
    error.clear();
    std::size_t nodes = 0;
    std::size_t stringBytes = 0;
    if (!AppendValue(value, output, 0, nodes, stringBytes, error))
    {
        output.clear();
        return false;
    }
    return true;
}

bool DecodeTypedStorageValue(std::string_view encoded,
    InteractionValue& output, std::string& error)
{
    output = {};
    error.clear();
    if (encoded.empty() ||
        encoded.size() > MaximumTypedStorageEncodedBytes ||
        !JsonNestingIsBounded(encoded))
    {
        error = "typed storage payload is empty, oversized, or too deeply nested";
        return false;
    }
    JsonValue root;
    if (!ParseJson(encoded, root, &error))
        return false;
    std::size_t nodes = 0;
    std::size_t stringBytes = 0;
    return ReadValue(root, output, 0, nodes, stringBytes, error);
}
}
