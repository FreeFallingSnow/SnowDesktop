#pragma once
#include <algorithm>

namespace snowdesktop::large_icon_render_rules
{
inline bool CanRevealTitle(float width, float height, float iconExtent, float titleSize, float scale)
{
    return width >= height * 1.2f &&
        width >= iconExtent + std::max(100.f * scale, titleSize * 4) + 36 * scale;
}
inline unsigned TitleBackdrop(unsigned textColor)
{
    const auto luma = ((textColor >> 16) & 255) * .2126 + ((textColor >> 8) & 255) * .7152 + (textColor & 255) * .0722;
    return luma < 140 ? 0xf2f4f7u : 0x20242bu;
}
}
