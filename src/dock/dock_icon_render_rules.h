#pragma once

#include <algorithm>
#include <cmath>
#include <windows.h>

namespace snowdesktop::dock_icon_render_rules
{
// The Dock has already applied hover growth to this frame and made room for
// it. Fit the artwork to that size even when the cached Shell bitmap is smaller.
// Keep media thumbnails proportional instead of stretching them into a square.
inline RECT BitmapDestination(RECT frame, int sourceWidth, int sourceHeight)
{
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    if (width <= 0 || height <= 0 || sourceWidth <= 0 || sourceHeight <= 0)
        return {};

    const double scale = (std::min)(
        static_cast<double>(width) / sourceWidth,
        static_cast<double>(height) / sourceHeight);
    const int fittedWidth = std::clamp(
        static_cast<int>(std::lround(sourceWidth * scale)), 1, width);
    const int fittedHeight = std::clamp(
        static_cast<int>(std::lround(sourceHeight * scale)), 1, height);
    const int left = frame.left + (width - fittedWidth) / 2;
    const int top = frame.top + (height - fittedHeight) / 2;
    return {left, top, left + fittedWidth, top + fittedHeight};
}
} // namespace snowdesktop::dock_icon_render_rules
