#pragma once
#include "tray_service.h"
#include <algorithm>

namespace snowdesktop::tray
{
// A fresh read of Explorer's current text is different from a late startup
// supplement. It can reconcile missed text updates, but cannot register or
// rebind an icon, nor change its negotiated callback, pixels or visibility.
inline bool RefreshNativeTooltip(std::vector<Icon>& icons, const Event& event)
{
    if (event.operation != kBootstrapIcon || !(event.flags & NIF_TIP)) return false;
    const auto found = std::find_if(icons.begin(), icons.end(), [&](const auto& icon) {
        return SameFocusIdentity(icon.identity, event.identity) &&
            icon.callback == event.callback && icon.version == event.version;
    });
    if (found == icons.end()) return false;
    const std::wstring tip(event.tip, wcsnlen_s(event.tip, std::size(event.tip)));
    if (found->tipKnown && found->tip == tip) return false;
    found->tip = tip; found->tipKnown = true;
    return true;
}
}
