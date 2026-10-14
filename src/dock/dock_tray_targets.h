#pragma once

#include "dock_tray_rules.h"
#include "system/tray/tray_service.h"

namespace snowdesktop::dock_tray
{
inline const DockTrayRule* TargetRule(std::span<const DockTrayRule> rules, std::wstring_view source,
    std::wstring_view target, std::span<const std::wstring> validatedPayloadPaths = {})
{
    const auto* original = FindRule(rules, source);
    if (!original || !original->enabled) return nullptr;
    if (Matches(*original, target)) return FindRule(rules, target);
    if (!original->matchPath || std::none_of(validatedPayloadPaths.begin(), validatedPayloadPaths.end(),
        [&](const auto& alias) { return Equal(alias, target); })) return nullptr;
    // A rule for the actual payload is more specific than its launcher rule.
    const auto* precise = FindRule(rules, target);
    return precise && precise->matchPath ? precise : original;
}

inline std::vector<tray::Icon> CollectTargets(const tray::Snapshot& snapshot,
    std::span<const DockTrayRule> rules, std::wstring_view executable,
    std::span<const std::wstring> validatedPayloadPaths = {})
{
    std::vector<tray::Icon> result;
    const auto* rule = FindRule(rules, executable);
    if (!snapshot.connected || !rule || !rule->enabled) return result;
    for (const auto& icon : snapshot.icons)
    {
        const auto* effective = TargetRule(rules, executable, icon.application, validatedPayloadPaths);
        if (effective && effective->enabled &&
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
