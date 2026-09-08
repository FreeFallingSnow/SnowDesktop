#pragma once
#include "large_icon_config.h"
#include <algorithm>

namespace snowdesktop::large_icon_render_rules
{
inline double Radius(const LargeIconConfig& c, double width, double height, double scale)
{
    const double maximum = std::max(0., std::min(width, height) / 2);
    return c.radiusPercent >= 0 ? maximum * c.radiusPercent / 100 : std::min(maximum, c.radius * scale);
}
inline double RadiusPercent(const LargeIconConfig& c, double width, double height, double scale)
{
    return c.radiusPercent >= 0 ? c.radiusPercent : std::min(width, height) > 0 ?
        Radius(c, width, height, scale) * 200 / std::min(width, height) : 0;
}
inline unsigned Mix(unsigned a, unsigned b, double amount)
{
    const auto channel = [&](int shift) { return static_cast<unsigned>(
        ((a >> shift) & 255) * (1 - amount) + ((b >> shift) & 255) * amount + .5); };
    return channel(16) << 16 | channel(8) << 8 | channel(0);
}
struct BackgroundStyle
{
    unsigned color = 0xe8ecf4; // DefaultBeautify base, available before any image arrives
    double opacity = 1;
    PanelGradient gradient;
};
inline BackgroundStyle DefaultBackground(const LargeIconConfig& c, unsigned accent, bool hasEdge, unsigned edge)
{
    BackgroundStyle result;
    if (c.smartFill && hasEdge) result.color = edge;
    else if (c.themeColor && accent)
    {
        result.color = accent; result.opacity = c.themeOpacity;
        result.gradient.enabled = c.themeGradient;
        result.gradient.angle = c.themeAngle;
        result.gradient.stops = {{0, accent, c.themeOpacity}, {1, Mix(accent, 0xe8ecf4, .4), c.themeOpacity}};
    }
    return result;
}
inline unsigned ReadableText(unsigned background)
{
    const double luma = ((background >> 16) & 255) * .2126 + ((background >> 8) & 255) * .7152 + (background & 255) * .0722;
    return luma >= 150 ? 0x161616u : 0xffffffu;
}
inline unsigned TextColor(const LargeIconConfig& c, unsigned background, unsigned componentForeground)
{
    return !c.autoTitleColor ? c.titleColor : c.backgroundStyle >= -1 ? componentForeground : ReadableText(background);
}
struct ContentGeometry
{
    double x = 0, y = 0, width = 0, height = 0;
    double sourceX = 0, sourceY = 0, sourceWidth = 0, sourceHeight = 0;
    double titleLeft = 0, titleTop = 0, titleWidth = 0, titleHeight = 0;
    bool leftReveal = false, upReveal = false, cropped = false;
};

inline ContentGeometry ResolveContent(const LargeIconConfig& c, double width, double height,
    double sourceWidth, double sourceHeight, double scale, bool original, double hover)
{
    ContentGeometry r;
    if (width <= 0 || height <= 0 || scale <= 0) return r;
    const bool fill = IsLargeIconFill(c);
    const double edge = std::max(1., std::min(width, height) * c.contentScale);
    sourceWidth = sourceWidth > 0 ? sourceWidth : edge;
    sourceHeight = sourceHeight > 0 ? sourceHeight : edge;
    r.sourceWidth = sourceWidth; r.sourceHeight = sourceHeight;
    if (fill && c.fit == 1)
    {
        const double factor = std::max(width / sourceWidth, height / sourceHeight);
        r.width = width; r.height = height; r.cropped = true;
        r.sourceWidth = width / factor; r.sourceHeight = height / factor;
        r.sourceX = (sourceWidth - r.sourceWidth) * c.focusX;
        r.sourceY = (sourceHeight - r.sourceHeight) * c.focusY;
    }
    else
    {
        const double factor = fill ? std::min(width / sourceWidth, height / sourceHeight) :
            std::min({original ? 1. : 1.e12, edge / sourceWidth, edge / sourceHeight});
        r.width = sourceWidth * factor; r.height = sourceHeight * factor;
        r.x = (width - r.width) * (fill || c.effect == 2 ? .5 : c.iconX);
        r.y = (height - r.height) * (fill || c.effect == 2 ? .5 : c.iconY);
    }
    if (c.effect != 2) return r;
    hover = std::clamp(hover, 0., 1.);
    const double size = c.revealTitleSize * scale, padding = 12 * scale;
    const double lines = std::ceil(size * 1.3 * 2);
    if (c.titleDirection == 0)
    {
        const double column = fill ? width * .55 : std::max(width * .4, r.width + 2 * padding);
        r.titleLeft = column + padding; r.titleWidth = width - r.titleLeft - padding;
        r.titleHeight = std::min(height - 2 * padding, lines);
        r.titleTop = (height - r.titleHeight) / 2;
        r.leftReveal = width >= height * 1.2 && r.titleWidth >= std::max(100 * scale, size * 4) && r.titleHeight >= size * 1.3;
        if (r.leftReveal) r.x += (fill ? -(width - column) : (column - r.width) / 2 - r.x) * hover;
    }
    else
    {
        const double column = fill ? height * .55 : std::max(height * .4, r.height + 2 * padding);
        const double available = height - column - 2 * padding;
        r.titleLeft = padding; r.titleWidth = width - 2 * padding;
        r.titleHeight = std::min(available, lines);
        r.titleTop = column + padding + (available - r.titleHeight) / 2;
        r.upReveal = r.titleWidth >= std::max(100 * scale, size * 4) && available >= size * 1.3;
        if (r.upReveal) r.y += (fill ? -(height - column) : (column - r.height) / 2 - r.y) * hover;
    }
    return r;
}
inline bool CanSelectTitleDirection(const LargeIconConfig& c, int direction, double width, double height,
    double sourceWidth, double sourceHeight, double scale)
{
    auto candidate = c; candidate.effect = 2; candidate.titleDirection = direction;
    const auto r = ResolveContent(candidate, width, height, sourceWidth, sourceHeight, scale,
        LargeIconActiveContent(c) == 0, 0);
    return direction == 0 ? r.leftReveal : r.upReveal;
}
}
