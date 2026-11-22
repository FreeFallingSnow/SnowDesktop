#pragma once

#include <windows.h>

namespace snowdesktop::dock_minimize
{
bool UsesNativeTransition(HWND window) noexcept;
}
