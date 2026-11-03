#pragma once

#include "settings/settings_domain.h"
#include "settings/settings_fwd.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "settings/settings_host_actions.h"
#include "home_about_page_model.h"
#include "hotkey_recorder_types.h"

namespace snowdesktop::winui
{

struct HomeAboutPageActions
{
    using GeneralEdit = std::function<void(GeneralSettings&)>;

    std::function<void(const SettingsRoute& route)> navigate;
    std::function<void(
        std::uint64_t generation,
        HomeAboutCommand command)> invoke;
    /** Opens one of the fixed, typed links from the legacy About page. */
    std::function<void(
        std::uint64_t generation,
        HomeAboutLink link)> openLink;
    /** Applies the persisted Debug demo-mode setting through the controller. */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        GeneralEdit edit)> updateGeneral;
    std::function<void(
        SettingsHostActions::HotkeyTarget, HotkeyChord, std::uint64_t,
        std::uint64_t, HotkeyAvailabilityCompletion)> probeHotkey;
    /** Toggles the non-persisted animation metrics for this application run. */
    std::function<void(
        std::uint64_t generation,
        bool enabled)> setAnimationDiagnostics;
    std::function<void(std::uint64_t generation, bool enabled)> setTemporaryInitialization;
    std::function<void(std::uint64_t, bool)> setDebugProfileEnabled;
    std::function<void(std::uint64_t)> chooseDebugDesktop;
    std::function<void(std::uint64_t)> clearDebugProfile;
    std::function<void(std::uint64_t, std::wstring)> selectDebugEnvironment;
    std::function<void(std::uint64_t)> createDebugEnvironment;
    std::function<void(std::uint64_t)> renameDebugEnvironment;
    /** Makes the conditional Debug route visible; true permits navigation. */
    std::function<bool(std::uint64_t generation)> unlockDebug;
    /** The host confirms and clears the local Steam unlock state. */
    std::function<void(std::uint64_t generation)>
        requestResetUnlockConfirmation;
    /** The host owns the HWND-scoped ContentDialog and crash-test action. */
    std::function<void(std::uint64_t generation)>
        requestCrashTestConfirmation;
};

} // namespace snowdesktop::winui
