#include "composition_motion_curve.h"
#include "layout/popup_motion_curve.h"

#include <dcomp.h>
#include <wrl/client.h>

namespace snowdesktop
{
HRESULT CreateCompositionMotionAnimation(IDCompositionDesktopDevice* device,
    const popup_motion_curve::Timeline& timeline, IDCompositionAnimation** animation,
    float origin, float extent)
{
    if (!animation) return E_INVALIDARG;
    *animation = nullptr;
    if (!device || timeline.segments.empty()) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> result;
    HRESULT hr = device->CreateAnimation(&result);
    for (const auto& segment : timeline.segments)
    {
        if (FAILED(hr)) break;
        hr = result->AddCubic(segment.startSeconds,
            static_cast<float>(origin + extent * segment.constant),
            static_cast<float>(extent * segment.linear),
            static_cast<float>(extent * segment.quadratic),
            static_cast<float>(extent * segment.cubic));
    }
    if (SUCCEEDED(hr)) hr = result->End(timeline.durationMilliseconds / 1000.0,
        static_cast<float>(origin + extent * timeline.target));
    if (SUCCEEDED(hr)) *animation = result.Detach();
    return hr;
}
}
