#pragma once

#include "dock_tray_rules.h"
#include "system/tray/tray_service.h"

namespace snowdesktop::dock_tray
{
inline std::vector<tray::Icon> CollectTargets(const tray::Snapshot& snapshot,
    std::span<const DockTrayRule> rules, std::wstring_view executable)
{
    std::vector<tray::Icon> result;
    const auto* rule = FindRule(rules, executable);
    if (!snapshot.connected || !rule || !rule->enabled) return result;
    for (const auto& icon : snapshot.icons)
    {
        const auto* effective = FindRule(rules, icon.application);
        if (effective && effective->enabled && Matches(*rule, icon.application) &&
            !(icon.state & NIS_HIDDEN) && icon.callback && icon.identity.window && icon.identity.process)
            result.push_back(icon);
    }
    return result;
}

inline bool IsCurrentTarget(const tray::Snapshot& snapshot, const tray::Icon& selected)
{
    return snapshot.connected && std::any_of(snapshot.icons.begin(), snapshot.icons.end(), [&](const auto& icon) {
        return icon.key == selected.key && tray::SameFocusIdentity(icon.identity, selected.identity) &&
            Equal(icon.application, selected.application) && !(icon.state & NIS_HIDDEN) && icon.callback;
    });
}
}
