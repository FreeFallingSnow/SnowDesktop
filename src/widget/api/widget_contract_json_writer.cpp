#include "widget_contract_json_writer.h"

#include <ostream>

namespace snowdesktop::widget_contract_json
{
void WriteJsonString(std::ostream& output, std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20)
            {
                output << "\\u00" << kHex[character >> 4]
                       << kHex[character & 0x0f];
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
}
}
