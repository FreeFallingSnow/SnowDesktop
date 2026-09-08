#pragma once
#include "large_icon_config.h"
#include <algorithm>
#include <functional>

namespace snowdesktop::large_icon_render_rules
{
inline LargeIconConfig ResolveComponentRadius(LargeIconConfig config, double componentRadius)
{
    if (config.followComponentRadius)
    {
        config.radius = componentRadius;
        config.radiusPercent = -1;
    }
    return config;
}
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
    if (c.backgroundStyle == -5) { result.opacity = .65; return result; }
    if (c.backgroundStyle == -4)
    {
        if (hasEdge) result.color = edge;
        else result.opacity = .65;
        return result;
    }
    if (c.backgroundStyle == 9)
    {
        result.color = c.manualColor; result.opacity = c.opacity; result.gradient = c.gradient;
        for (auto& stop : result.gradient.stops) stop.opacity *= c.gradientOpacity;
        return result;
    }
    if (c.defaultBackground == 1)
    {
        result.gradient = c.defaultGradient;
        if (!result.gradient.enabled)
        {
            const auto color = accent ? accent : result.color;
            result.gradient.angle = c.themeAngle;
            result.gradient.stops = {{0, color, c.themeOpacity}, {1, Mix(color, 0xe8ecf4, .4), c.themeOpacity}};
        }
        result.gradient.enabled = true;
        result.color = result.gradient.stops.front().color;
        result.opacity = result.gradient.stops.front().opacity;
    }
    else if (c.defaultBackground == 2)
    {
        result.color = c.defaultSolidColor; result.opacity = c.defaultSolidOpacity;
    }
    else if (c.smartFill && hasEdge) result.color = edge;
    else result.opacity = c.themeOpacity;
    return result;
}
inline unsigned ReadableText(unsigned background)
{
    const double luma = ((background >> 16) & 255) * .2126 + ((background >> 8) & 255) * .7152 + (background & 255) * .0722;
    return luma >= 150 ? 0x161616u : 0xffffffu;
}
inline unsigned TextColor(const LargeIconConfig& c, unsigned background, unsigned componentForeground)
{
    return !c.autoTitleColor ? c.titleColor : (c.backgroundStyle >= -1 || (c.backgroundStyle == -3 && c.defaultBackground != 0)) ? componentForeground : ReadableText(background);
}
struct ContentGeometry
{
    double x = 0, y = 0, width = 0, height = 0;
    double sourceX = 0, sourceY = 0, sourceWidth = 0, sourceHeight = 0;
    double titleLeft = 0, titleTop = 0, titleWidth = 0, titleHeight = 0;
    bool leftReveal = false, upReveal = false, cropped = false;
};
struct TitleSize { double width = 0, height = 0; bool fits = false; };
using MeasureTitle = std::function<TitleSize(double, double)>;

inline ContentGeometry ResolveContent(const LargeIconConfig& c, double width, double height,
    double sourceWidth, double sourceHeight, double scale, bool original, double hover,
    const MeasureTitle& measure = {})
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
        const double factor = std::max(width / sourceWidth, height / sourceHeight) * c.fillScale;
        r.width = std::min(width, sourceWidth * factor); r.height = std::min(height, sourceHeight * factor); r.cropped = true;
        r.x = (width - r.width) / 2; r.y = (height - r.height) / 2;
        r.sourceWidth = r.width / factor; r.sourceHeight = r.height / factor;
        r.sourceX = (sourceWidth - r.sourceWidth) * c.focusX;
        r.sourceY = (sourceHeight - r.sourceHeight) * c.focusY;
    }
    else
    {
        const double factor = fill ? std::min(width / sourceWidth, height / sourceHeight) * c.fillScale :
            std::min({original ? 1. : 1.e12, edge / sourceWidth, edge / sourceHeight});
        r.width = sourceWidth * factor; r.height = sourceHeight * factor;
        r.x = (width - r.width) * (fill || c.effect == 2 ? .5 : c.iconX);
        r.y = (height - r.height) * (fill || c.effect == 2 ? .5 : c.iconY);
        if (fill)
        {
            // Flatten any overscan before title motion, including zoomed contain.
            r.sourceWidth = std::min(width, r.width) / factor;
            r.sourceHeight = std::min(height, r.height) / factor;
            r.sourceX = (sourceWidth - r.sourceWidth) / 2;
            r.sourceY = (sourceHeight - r.sourceHeight) / 2;
            r.width = std::min(width, r.width); r.height = std::min(height, r.height);
            r.x = (width - r.width) / 2; r.y = (height - r.height) / 2; r.cropped = true;
        }
    }
    if (c.effect != 2 || fill) return r;
    hover = std::clamp(hover, 0., 1.);
    const double preferredPadding = 12 * scale, preferredGap = 12 * scale;
    const auto candidate = [&](bool left, double padding, double gap) {
        auto result = r;
        const double maxWidth = left ? (fill ? width * .5 - 2 * padding : width - r.width - gap - 2 * padding) : width - 2 * padding;
        const double maxHeight = left ? height - 2 * padding : (fill ? height * .5 - 2 * padding : height - r.height - gap - 2 * padding);
        if (!measure || maxWidth <= 0 || maxHeight <= 0 ||
            (!fill && (left ? r.height > height + .01 : r.width > width + .01))) return result;
        const auto text = measure(maxWidth, maxHeight);
        if (!text.fits || text.width <= 0 || text.height <= 0) return result;
        result.titleWidth = text.width; result.titleHeight = text.height;
        if (left)
        {
            const double start = fill ? width - padding - text.width - gap - r.width : (width - r.width - gap - text.width) / 2;
            result.titleLeft = fill ? width - padding - text.width : start + r.width + gap;
            result.titleTop = (height - text.height) / 2;
            result.x += (std::min(start, r.x) - r.x) * hover;
            result.leftReveal = true;
        }
        else
        {
            const double start = fill ? height - padding - text.height - gap - r.height : (height - r.height - gap - text.height) / 2;
            result.titleLeft = (width - text.width) / 2;
            result.titleTop = fill ? height - padding - text.height : start + r.height + gap;
            result.y += (std::min(start, r.y) - r.y) * hover;
            result.upReveal = true;
        }
        return result;
    };
    const bool left = c.autoTitleDirection ? width > height * 1.15 : c.titleDirection == 0;
    auto choose = [&](bool direction) {
        auto result = candidate(direction, preferredPadding, preferredGap);
        if (!result.leftReveal && !result.upReveal)
            result = candidate(direction, 2 * scale, 4 * scale);
        return result;
    };
    auto chosen = choose(left);
    if (c.autoTitleDirection && !chosen.leftReveal && !chosen.upReveal) chosen = choose(!left);
    return chosen;
}
inline bool CanSelectTitleDirection(const LargeIconConfig& c, int direction, double width, double height,
    double sourceWidth, double sourceHeight, double scale, const MeasureTitle& measure = {})
{
    auto candidate = c; candidate.effect = 2; candidate.autoTitleDirection = direction == 2; candidate.titleDirection = direction == 2 ? 0 : direction;
    const auto r = ResolveContent(candidate, width, height, sourceWidth, sourceHeight, scale,
        LargeIconActiveContent(c) == 0, 0, measure);
    return direction == 2 ? r.leftReveal || r.upReveal : direction == 0 ? r.leftReveal : r.upReveal;
}
}
