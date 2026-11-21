#pragma once

#include <windows.h>

struct IDCompositionAnimation;
struct IDCompositionDesktopDevice;

namespace snowdesktop
{
namespace popup_motion_curve { struct Timeline; }

HRESULT CreateCompositionMotionAnimation(IDCompositionDesktopDevice* device,
    const popup_motion_curve::Timeline& timeline, IDCompositionAnimation** animation,
    float origin = 0.0f, float extent = 1.0f);
}
