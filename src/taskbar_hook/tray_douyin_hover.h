#pragma once

#include "tray_menu_first_show.h"

namespace snowdesktop::tray::first_show::douyin
{
bool Arm(const Request& request, DWORD host);
void Bind(HWND popup);
}
