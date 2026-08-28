#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::widget_scroll_rules
{

struct WheelResult
{
    int offset = 0;
    bool moved = false;
    bool reachedEnd = false;
};

struct ScrollbarAxisGeometry
{
    int trackStart = 0;
    int trackEnd = 0;
    int thumbStart = 0;
    int thumbEnd = 0;
    int maximum = 0;

    int TrackExtent() const noexcept
    {
        return std::max(0, trackEnd - trackStart);
    }

    int ThumbExtent() const noexcept
    {
        return std::max(0, thumbEnd - thumbStart);
    }

    int ThumbTravel() const noexcept
    {
        return std::max(0, TrackExtent() - ThumbExtent());
    }

    bool CanDrag() const noexcept
    {
        return maximum > 0 && ThumbTravel() > 0;
    }
};

inline ScrollbarAxisGeometry ResolveScrollbarAxisGeometry(
    int viewportStart, int viewportEnd, int contentExtent,
    int visibleExtent, int scrollOffset, float scale = 1.0f) noexcept
{
    ScrollbarAxisGeometry result;
    const int viewportExtent = std::max(0, viewportEnd - viewportStart);
    visibleExtent = std::max(0, visibleExtent);
    result.maximum = std::max(0, contentExtent - visibleExtent);
    if (viewportExtent <= 0 || visibleExtent <= 0 || result.maximum <= 0)
        return result;

    const int requestedInset = std::max(
        1, static_cast<int>(std::lround(4.0f * std::max(0.1f, scale))));
    const int inset = std::min(requestedInset,
        std::max(0, (viewportExtent - 1) / 2));
    result.trackStart = viewportStart + inset;
    result.trackEnd = viewportEnd - inset;
    const int trackExtent = result.TrackExtent();
    if (trackExtent <= 0)
        return result;

    const float ratio = std::clamp(
        static_cast<float>(visibleExtent) /
            static_cast<float>(std::max(1, contentExtent)),
        0.08f, 1.0f);
    const int minimumThumb = std::max(
        8, static_cast<int>(std::lround(
            20.0f * std::max(0.1f, scale))));
    const int thumbExtent = std::min(trackExtent,
        std::max(minimumThumb,
            static_cast<int>(trackExtent * ratio)));
    const int thumbTravel = std::max(0, trackExtent - thumbExtent);
    const float scrollRatio = std::clamp(
        static_cast<float>(scrollOffset) /
            static_cast<float>(result.maximum),
        0.0f, 1.0f);
    result.thumbStart = result.trackStart +
        static_cast<int>(thumbTravel * scrollRatio);
    result.thumbEnd = result.thumbStart + thumbExtent;
    return result;
}

inline bool ScrollbarThumbHit(
    const ScrollbarAxisGeometry& geometry, int primaryCoordinate,
    int crossCoordinate, int viewportCrossEnd,
    float scale = 1.0f) noexcept
{
    if (!geometry.CanDrag()) return false;
    const int hitThickness = std::max(
        10, static_cast<int>(std::lround(
            12.0f * std::max(0.1f, scale))));
    return primaryCoordinate >= geometry.thumbStart &&
        primaryCoordinate < geometry.thumbEnd &&
        crossCoordinate >= viewportCrossEnd - hitThickness &&
        crossCoordinate < viewportCrossEnd;
}

inline int ApplyScrollbarThumbDrag(
    int startOffset, int pointerDelta,
    const ScrollbarAxisGeometry& geometry) noexcept
{
    if (!geometry.CanDrag())
        return std::clamp(startOffset, 0, geometry.maximum);
    const double delta = static_cast<double>(pointerDelta) *
        static_cast<double>(geometry.maximum) /
        static_cast<double>(geometry.ThumbTravel());
    return std::clamp(
        startOffset + static_cast<int>(std::lround(delta)),
        0, geometry.maximum);
}

inline bool ReachedScrollEnd(int previousOffset, int currentOffset,
    int maximum) noexcept
{
    maximum = std::max(0, maximum);
    return previousOffset < maximum && currentOffset == maximum;
}

inline WheelResult ApplyWheelDelta(
    int offset, int maximum, int delta, int step = 48)
{
    constexpr int kWheelDelta = 120;
    maximum = std::max(0, maximum);
    int normalizedStep =
        delta == 0
            ? 0
            : (delta * step) / kWheelDelta;
    if (delta != 0 && normalizedStep == 0)
        normalizedStep = delta > 0 ? 1 : -1;
    const int next =
        std::clamp(offset - normalizedStep, 0, maximum);
    return { next, next != offset,
        ReachedScrollEnd(offset, next, maximum) };
}

} // namespace snowdesktop::widget_scroll_rules
