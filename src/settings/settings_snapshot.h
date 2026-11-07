#pragma once

#include "settings_domain.h"
#include "settings_route.h"
#include "settings_values.h"

#include <cstdint>
#include <string>

namespace snowdesktop
{

/** Per-domain revisions used by views to update only changed presenters. */
struct SettingsDomainRevisions
{
    std::uint64_t personalization = 0;
    std::uint64_t dock = 0;
    // Windows-owned taskbar fields may change while an unrelated Dock draft
    // is pending. Keep their presenter revision independent from the coarse
    // Dock domain so that reconciliation does not repaint local slider drafts.
    std::uint64_t systemTaskbar = 0;
    std::uint64_t navigation = 0;
    std::uint64_t general = 0;
    std::uint64_t category = 0;
    std::uint64_t desktop = 0;
};

/**
 * Immutable value published to a settings ViewModel.
 *
 * revision changes for every observable controller transition. generation
 * changes whenever a view session ends or persisted state is reloaded, and is
 * the token asynchronous work must validate before applying its result.
 */
struct SettingsSnapshot
{
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    bool initialized = false;
    bool sessionActive = false;
    SettingsRoute route;
    SettingsValues values;
    SettingsDomainRevisions domainRevisions;
    SettingsDomain dirtyDomains = SettingsDomain::None;
    SettingsDomain pendingPreviewDomains = SettingsDomain::None;
    SettingsDomain pendingCommitDomains = SettingsDomain::None;
    bool retryRequired = false;
    /**
     * A complete data-tree replacement has been committed outside the
     * controller. The current process must not publish its stale in-memory
     * values again; only restart and exit host actions remain valid.
     */
    bool externalReplacementPending = false;
    std::wstring lastActionMessage;
};

} // namespace snowdesktop
