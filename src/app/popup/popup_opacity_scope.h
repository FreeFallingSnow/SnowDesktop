#pragma once

#include "ui/render/scroll_content_clip.h"

// Fade both cached and directly drawn popup content, including early returns.
class PopupOpacityScope
{
public:
    PopupOpacityScope(ID2D1DeviceContext* context, bool enabled, float opacity)
        : opacityLayer_(enabled && opacity < 1.0f ? context : nullptr, D2D1::InfiniteRect(), opacity)
    {}
    PopupOpacityScope(const PopupOpacityScope&) = delete;
    PopupOpacityScope& operator=(const PopupOpacityScope&) = delete;
private:
    snowdesktop::ScopedContentOpacityLayer opacityLayer_;
};
