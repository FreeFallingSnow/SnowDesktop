#pragma once
#include "large_icon_config.h"
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

inline unsigned Background(const LargeIconConfig& config, unsigned neutral, unsigned accent)
{
    if (!config.autoColor) return config.manualColor;
    // Extracted channels are bounded away from zero; zero denotes that there
    // were no usable visible pixels, so the active theme supplies the fallback.
    if (!accent) accent = neutral;
    const auto channel = [&](int shift) { return static_cast<unsigned>(
        ((neutral >> shift) & 255) * (1 - config.colorMix) + ((accent >> shift) & 255) * config.colorMix); };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

struct ContentGeometry
{
    double x = 0, y = 0, width = 0, height = 0;
    double titleLeft = 0;
    bool leftReveal = false;
};

inline ContentGeometry ResolveContent(const LargeIconConfig& config, double width, double height,
    double sourceWidth, double sourceHeight, double scale, bool original, bool animations,
    double hover, bool pressed, double launchWave)
{
    const double edge = std::max(1., std::min(width, height) * config.contentScale);
    sourceWidth = sourceWidth > 0 ? sourceWidth : edge;
    sourceHeight = sourceHeight > 0 ? sourceHeight : edge;
    const double factor = original ? std::min({1., edge / sourceWidth, edge / sourceHeight}) :
        config.fit == 0 ? std::min(width / sourceWidth, height / sourceHeight) : std::max(width / sourceWidth, height / sourceHeight);
    const double baseWidth = sourceWidth * factor;
    ContentGeometry result;
    result.leftReveal = original && config.content == 0 && animations && (config.titleMode == 1 || config.hoverContent == 1) &&
        CanRevealTitle(static_cast<float>(width), static_cast<float>(height), static_cast<float>(baseWidth),
            static_cast<float>(config.titleSize * scale), static_cast<float>(scale));
    double zoom = 1;
    if (animations && ((!original && config.coverHover == 1) || (original && !result.leftReveal && config.hoverContent == 2)))
        zoom += .06 * hover * config.amplitude;
    if (animations && pressed && config.press) zoom *= .96;
    result.width = sourceWidth * factor * zoom; result.height = sourceHeight * factor * zoom;
    result.x = (width - result.width) * (original || config.fit == 0 ? .5 : config.focusX);
    result.y = (height - result.height) * (original || config.fit == 0 ? .5 : config.focusY);
    if (result.leftReveal) result.x += (12 * scale - result.x) * hover;
    else if (animations && original && config.hoverContent == 3) result.y -= 6 * scale * hover * config.amplitude;
    if (animations && config.launch == 1) result.y -= 9 * scale * launchWave * config.amplitude;
    result.titleLeft = baseWidth + 24 * scale;
    return result;
}
}
