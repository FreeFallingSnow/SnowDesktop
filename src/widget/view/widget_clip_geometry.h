#pragma once
#include <d2d1_1.h>
#include <wrl/client.h>
#include <utility>
#include "ui/render/smooth_corner_geometry.h"

namespace snowdesktop
{
// Shared by native and Lua widget clipping. The cache belongs to the widget,
// so it can outlive a device rebuild or a change of rendering factory.
inline ID2D1Geometry* ResolveWidgetClipGeometry(
    ID2D1Factory1* factory, const RECT& frame, float radius,
    Microsoft::WRL::ComPtr<ID2D1Geometry>& cachedClipGeometry_,
    RECT& cachedClipFrame_, float& cachedClipRadius_)
{
    if (!factory) return nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> owner;
    if (cachedClipGeometry_) cachedClipGeometry_->GetFactory(&owner);
    if (cachedClipGeometry_ && owner.Get() == factory &&
        cachedClipFrame_.left == frame.left &&
        cachedClipFrame_.top == frame.top &&
        cachedClipFrame_.right == frame.right &&
        cachedClipFrame_.bottom == frame.bottom &&
        cachedClipRadius_ == radius)
        return cachedClipGeometry_.Get();

    auto geo = smooth_corners::Geometry(factory,
        D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
            static_cast<float>(frame.right), static_cast<float>(frame.bottom)), radius);
    if (!geo) return nullptr;
    cachedClipGeometry_ = geo;
    cachedClipFrame_ = frame;
    cachedClipRadius_ = radius;
    return cachedClipGeometry_.Get();
}
}
