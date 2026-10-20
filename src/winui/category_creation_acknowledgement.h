#pragma once

#include "settings/settings_controller.h"

#include <algorithm>
#include <string_view>

namespace snowdesktop::winui
{
// Creation appears in draft snapshots before persistence runs. Only a clean
// snapshot from the same session acknowledges that the new rule was saved.
inline bool CanAcknowledgeCategoryCreation(const SettingsSnapshot& snapshot,
    std::wstring_view pendingId, std::uint64_t expectedGeneration)
{
    if (pendingId.empty() || !snapshot.initialized || !snapshot.sessionActive ||
        snapshot.externalReplacementPending || snapshot.generation != expectedGeneration ||
        HasSettingsDomain(snapshot.dirtyDomains, SettingsDomain::Category) ||
        HasSettingsDomain(snapshot.pendingCommitDomains, SettingsDomain::Category) ||
        HasSettingsDomain(snapshot.pendingPreviewDomains, SettingsDomain::Category))
        return false;

    return std::any_of(snapshot.values.category.rules.begin(), snapshot.values.category.rules.end(),
        [&](const CategoryRule& rule) { return rule.id == pendingId; });
}
}
