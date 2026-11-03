#pragma once

#include "settings/settings_domain.h"
#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "settings/settings_host_actions.h"
#include "general_page_types.h"
#include "general_advanced_feature_status.h"
#include "start_page_actions.h"
#include "hotkey_recorder_types.h"
#include "platform/window_support/window_capabilities.h"

namespace snowdesktop::winui
{

struct GeneralPageActions
{
    StartPageActions onboarding;
    std::function<void(const SettingsRoute&)> navigate;
    using GeneralEdit = std::function<void(GeneralSettings&)>;
    using NavigationEdit = std::function<void(NavigationSettings&)>;
    using DockEdit = std::function<void(DockSettings&)>;

    std::function<void(std::uint64_t generation, GeneralEdit edit)>
        commitGeneral;
    std::function<void(std::uint64_t generation, NavigationEdit edit)>
        commitNavigation;
    std::function<void(std::uint64_t generation, DockEdit edit)>
        commitDock;

    /** Auto-start is a Windows/MSIX projection, not a GeneralSettings edit. */
    std::function<void(std::uint64_t generation, bool enabled)>
        setAutoStart;
    std::function<GeneralStartupConflict()> queryStartupConflict;
    std::function<window_support::SupportStatus()> queryWindowSupport;
    std::function<void(bool)> configureWindowSupport;
    std::function<GeneralAdvancedFeatureStatus()> queryAdvancedFeatureStatus;
    std::function<void()> registerAdvancedFeatures;
    std::function<void()> openAdvancedFeaturesStore;

    std::function<void(
        SettingsHostActions::HotkeyTarget target,
        HotkeyChord chord,
        std::uint64_t generation,
        std::uint64_t requestId,
        HotkeyAvailabilityCompletion completion)>
        probeHotkey;

    std::function<std::vector<SettingsLanguageOption>()> languageCatalog;
};

} // namespace snowdesktop::winui
