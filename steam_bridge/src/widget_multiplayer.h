// SPDX-License-Identifier: MIT
#pragma once

namespace snowdesktop::steam_bridge
{
// Called with a verified Steam API session on the callback thread.
int RunWidgetMultiplayer(int argc, wchar_t** argv);
}
