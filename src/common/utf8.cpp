#include "utf8.h"

#include <cstddef>
#include <cstdint>

namespace snowdesktop::utf8
{
bool IsValid(std::string_view value) noexcept
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
}
