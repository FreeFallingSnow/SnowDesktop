#pragma once

#include <string>
#include <string_view>

namespace snowdesktop::json
{
// Contents of a JSON string, without the surrounding quotes.
inline std::string EscapeString(std::string_view input)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size());
    for (const unsigned char value : input)
    {
        if (value == '"' || value == '\\')
        {
            output += '\\';
            output += static_cast<char>(value);
        }
        else if (value < 0x20)
        {
            output += "\\u00";
            output += hex[value >> 4];
            output += hex[value & 15];
        }
        else output += static_cast<char>(value);
    }
    return output;
}
}
