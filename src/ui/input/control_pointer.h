#pragma once

#include <windows.h>

namespace snowdesktop::controlled_pointer
{
// Pure geometry shared by preview timelines and native private-device paths.
POINT Interpolate(POINT from, POINT to, double progress, double curvature = 0.12);
bool AbsolutePoint(POINT point, RECT virtualScreen, LONG& x, LONG& y);
}