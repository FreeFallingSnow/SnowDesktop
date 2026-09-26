#pragma once
#include <d2d1helper.h>
#include <algorithm>

namespace snowdesktop::native_controls
{
struct SliderGeometry
{
    D2D1_RECT_F track{}, fill{};
    D2D1_POINT_2F thumb{};
    float trackRadius = 2;
};
// Shared by Lua controls and native system panels. Content bounds and thumb
// radius are supplied by the caller's layout; rendering and input share axes.
inline SliderGeometry Slider(D2D1_RECT_F content, float thumbRadius,
    float trackThickness, float normalized, bool vertical)
{
    SliderGeometry result;
    const float length=vertical?content.bottom-content.top:content.right-content.left;
    const float inset=(std::min)(thumbRadius,(std::max)(0.f,length*.5f));
    const float cross=vertical?(content.left+content.right)*.5f:(content.top+content.bottom)*.5f;
    result.track=vertical?D2D1::RectF(cross-trackThickness*.5f,content.top+inset,cross+trackThickness*.5f,content.bottom-inset):
        D2D1::RectF(content.left+inset,cross-trackThickness*.5f,content.right-inset,cross+trackThickness*.5f);
    result.fill=result.track;result.trackRadius=trackThickness*.5f;normalized=std::clamp(normalized,0.f,1.f);
    if(vertical){const float y=result.track.bottom-normalized*(result.track.bottom-result.track.top);result.fill.top=y;result.thumb={cross,y};}
    else{const float x=result.track.left+normalized*(result.track.right-result.track.left);result.fill.right=x;result.thumb={x,cross};}
    return result;
}
}
