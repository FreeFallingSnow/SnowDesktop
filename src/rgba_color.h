#pragma once

#include <windows.h>
#include <d2d1.h>
#include <string>
#include <string_view>

namespace snowdesktop
{
// Internal rendering color. Win32 callers retain their RGB conversion while
// composition painters preserve opacity.
struct RgbaColor
{
    COLORREF rgb;
    float alpha;
    constexpr RgbaColor(COLORREF value = 0, float opacity = 1.f) : rgb(value), alpha(opacity) {}
    constexpr operator COLORREF() const { return rgb; }
    bool operator==(const RgbaColor&) const = default;
};
inline bool DecodeRgbaColor(std::string_view text, RgbaColor& color)
{
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') return false;
    unsigned value = 0;
    for (const char character : text.substr(1))
    {
        const unsigned digit = character >= '0' && character <= '9' ? static_cast<unsigned>(character - '0') :
            character >= 'a' && character <= 'f' ? static_cast<unsigned>(character - 'a' + 10) :
            character >= 'A' && character <= 'F' ? static_cast<unsigned>(character - 'A' + 10) : 16;
        if (digit > 15) return false;
        value = (value << 4) | digit;
    }
    const unsigned rgb = text.size() == 9 ? value >> 8 : value;
    color = {RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255),
        text.size() == 9 ? static_cast<float>(value & 255) / 255.f : 1.f};
    return true;
}
inline std::string EncodeRgbaColor(unsigned red, unsigned green, unsigned blue, unsigned alpha)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string value = "#00000000";
    const unsigned channels[]{red, green, blue, alpha};
    for (unsigned i = 0; i < 4; ++i)
    {
        value[1 + i * 2] = hex[(channels[i] >> 4) & 15];
        value[2 + i * 2] = hex[channels[i] & 15];
    }
    return value;
}
}
inline D2D1_COLOR_F ToD2DColor(snowdesktop::RgbaColor color, float opacity = 1.f)
{
    return {GetRValue(color.rgb) / 255.f, GetGValue(color.rgb) / 255.f,
        GetBValue(color.rgb) / 255.f, color.alpha * opacity};
}
