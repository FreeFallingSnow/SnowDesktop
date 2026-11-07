#pragma once

#include "common/constants.h"
#include <windows.h>
#include <algorithm>
#include <cstddef>

namespace snowdesktop::collection_popup_layout
{
struct Metrics
{
    float scale = 1.0f;
    int cellWidth = kCellWidth;
    int cellHeight = kMinCellHeight;
    int minimumListHeight = 1;
    int paddingX = kCollectionPopupPaddingX;
    int headerHeight = kCollectionPopupHeaderHeight;
    int bottomPadding = kCollectionPopupBottomPadding;
    int gapX = kCollectionPopupGapX;
    int gapY = kCollectionPopupGapY;
    int edgeMargin = 12;
    int anchorGap = 12;
    int maximumWidth = 560;
    int maximumHeight = 640;
};

struct HeaderVerticalBounds
{
    int titleTop = 18;
    int titleBottom = 44;
    int sortButtonTop = 14;
    int sortButtonBottom = 48;
};

struct FanPoint { float x = 0; float y = 0; };

struct FanItem
{
    RECT icon{};
    RECT label{};
    FanPoint center{};
    float angle = 0;
};

struct FanLine { FanPoint start, end; };

struct FanRange { std::size_t first = 0, end = 0; };

struct FanScrollState
{
    double position = 0, target = 0, from = 0;
    double started = 0, duration = 0;

    bool Advance(double now)
    {
        const double t = duration > 0 ? std::clamp((now - started) / duration, 0.0, 1.0) : 1.0;
        const double rest = 1.0 - t;
        position = from + (target - from) * (1.0 - rest * rest * rest);
        if (t >= 1.0) position = target;
        return t < 1.0 && from != target;
    }
    void Clamp(double maximum)
    {
        maximum = std::max(0.0, maximum);
        position = std::clamp(position, 0.0, maximum);
        from = std::clamp(from, 0.0, maximum);
        target = std::clamp(target, 0.0, maximum);
    }
    void MoveTo(double value, double maximum, double now, double milliseconds)
    {
        Advance(now);
        Clamp(maximum);
        value = std::clamp(value, 0.0, std::max(0.0, maximum));
        if (value == target && milliseconds > 0) return;
        from = position;
        target = value;
        started = now;
        duration = milliseconds;
        if (duration <= 0) position = from = target;
    }
};
}
