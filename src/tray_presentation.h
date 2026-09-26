#pragma once
#include "tray_service.h"

namespace snowdesktop::tray
{
// Windows system-icon identities, also used by YASB's pinned systray.py.
// Presentation only: keep collecting these icons and preserve saved pins.
inline bool DuplicatesControlCenter(const Icon& icon)
{
    const auto& id = icon.identity.guid;
    constexpr unsigned char tail[]{0x82, 0xc1, 0xe4, 0x1c, 0xb6, 0x7d, 0x5b, 0x9c};
    return id.Data1 >= 0x7820ae73 && id.Data1 <= 0x7820ae75 &&
        id.Data2 == 0x23e3 && id.Data3 == 0x4229 &&
        std::equal(std::begin(id.Data4), std::end(id.Data4), std::begin(tail));
}
}
