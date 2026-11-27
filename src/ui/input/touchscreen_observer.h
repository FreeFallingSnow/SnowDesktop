#pragma once

#include <Windows.h>
#include <optional>

namespace snowdesktop::touch_input
{
void RegisterTouchscreenObserver(HWND target) noexcept;
void ForgetTouchscreenDevice(HANDLE device) noexcept;
std::optional<POINT> ReadTouchscreenPress(HRAWINPUT input);
}
