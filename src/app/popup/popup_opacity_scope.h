#pragma once

#include <d2d1_1.h>
#include <d2d1_1helper.h>

// Fade both cached and directly drawn popup content, including early returns.
class PopupOpacityScope
{
public:
    PopupOpacityScope(ID2D1DeviceContext* context, bool enabled, float opacity)
        : context_(enabled && opacity < 1.0f ? context : nullptr)
    {
        if (context_)
            context_->PushLayer(D2D1::LayerParameters1(
                D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(), opacity), nullptr);
    }
    ~PopupOpacityScope() { if (context_) context_->PopLayer(); }
    PopupOpacityScope(const PopupOpacityScope&) = delete;
    PopupOpacityScope& operator=(const PopupOpacityScope&) = delete;
private:
    ID2D1DeviceContext* context_;
};
