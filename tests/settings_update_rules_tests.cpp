#include "settings_update_rules.h"

#include <cstdlib>
#include <iostream>

// This rule-only target intentionally does not link the native
// personalization implementation. Supply the value factory used by
// DockSettings aggregate default construction, matching the controller tests.
PersonalizationSettings PersonalizationSettings::AcrylicDarkPreset()
{
    return {};
}

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
}

int main()
{
    using snowdesktop::dock_settings_rules::
        DisableSummonOnlyWhenPrerequisiteDisabled;
    using snowdesktop::dock_settings_rules::NormalizeSummonOnlyDependencies;
    using snowdesktop::settings_update_rules::IsGeneralShortcutOnlyCommit;
    using snowdesktop::settings_update_rules::IsFloatingDockShortcutOnlyCommit;
    using snowdesktop::settings_update_rules::IsNavigationShortcutOnlyCommit;
    using snowdesktop::settings_update_rules::ParseGitHubRelease;

    GeneralSettings general;
    GeneralSettings generalHotkey = general;
    generalHotkey.pageNavigationPreviousVirtualKey = VK_LEFT;
    Check(IsGeneralShortcutOnlyCommit(general, generalHotkey),
        "a page-navigation chord is isolated from the full General refresh");
    generalHotkey.demoModeEnabled = !general.demoModeEnabled;
    Check(!IsGeneralShortcutOnlyCommit(general, generalHotkey),
        "a visual General change still uses the full commit pipeline");

    NavigationSettings navigation;
    NavigationSettings navigationHotkey = navigation;
    navigationHotkey.virtualKey = 'N';
    Check(IsNavigationShortcutOnlyCommit(navigation, navigationHotkey),
        "a quick-navigation chord is isolated from desktop refresh");
    navigationHotkey.desktopViewMode = QuickNavigationDesktopViewMode::Source;
    Check(!IsNavigationShortcutOnlyCommit(navigation, navigationHotkey),
        "a navigation content change still uses the full commit pipeline");

    DockSettings dock;
    Check(!dock.allowDesktopContentOverlap &&
            !dock.showOnlyWhenSummoned,
        "desktop overlap and summon-only Dock display are opt-in by default");
    DockSettings normalizedSummonOnly = dock;
    normalizedSummonOnly.floatingEdgeSwipeEnabled = false;
    normalizedSummonOnly.showOnlyWhenSummoned = true;
    NormalizeDockSettings(normalizedSummonOnly);
    Check(normalizedSummonOnly.showOnlyWhenSummoned &&
            normalizedSummonOnly.allowDesktopContentOverlap &&
            normalizedSummonOnly.floatingEdgeSwipeEnabled,
        "normalization makes both reveal prerequisites follow summon-only Dock display");

    bool showOnlyWhenSummoned = false;
    bool allowDesktopContentOverlap = false;
    bool floatingEdgeSwipeEnabled = false;
    showOnlyWhenSummoned = true;
    NormalizeSummonOnlyDependencies(showOnlyWhenSummoned,
        allowDesktopContentOverlap, floatingEdgeSwipeEnabled);
    Check(showOnlyWhenSummoned && allowDesktopContentOverlap &&
            floatingEdgeSwipeEnabled,
        "summon-only Dock display enables overlap and edge-swipe reveal");
    showOnlyWhenSummoned = false;
    NormalizeSummonOnlyDependencies(showOnlyWhenSummoned,
        allowDesktopContentOverlap, floatingEdgeSwipeEnabled);
    Check(!showOnlyWhenSummoned && allowDesktopContentOverlap &&
            floatingEdgeSwipeEnabled,
        "disabling summon-only display preserves its useful prerequisites");
    allowDesktopContentOverlap = true;
    DisableSummonOnlyWhenPrerequisiteDisabled(
        allowDesktopContentOverlap, showOnlyWhenSummoned);
    Check(allowDesktopContentOverlap && !showOnlyWhenSummoned,
        "enabling one prerequisite alone does not enable summon-only display");
    showOnlyWhenSummoned = true;
    allowDesktopContentOverlap = false;
    DisableSummonOnlyWhenPrerequisiteDisabled(
        allowDesktopContentOverlap, showOnlyWhenSummoned);
    Check(!allowDesktopContentOverlap && !showOnlyWhenSummoned,
        "disabling desktop overlap also disables summon-only display");
    showOnlyWhenSummoned = true;
    floatingEdgeSwipeEnabled = false;
    DisableSummonOnlyWhenPrerequisiteDisabled(
        floatingEdgeSwipeEnabled, showOnlyWhenSummoned);
    Check(!floatingEdgeSwipeEnabled && !showOnlyWhenSummoned,
        "disabling edge-swipe reveal also disables summon-only display");

    DockSettings dockBehavior = dock;
    dockBehavior.allowDesktopContentOverlap = true;
    Check(!IsFloatingDockShortcutOnlyCommit(dock, dockBehavior),
        "desktop overlap uses the full Dock commit pipeline");
    dockBehavior = dock;
    dockBehavior.showOnlyWhenSummoned = true;
    Check(!IsFloatingDockShortcutOnlyCommit(dock, dockBehavior),
        "summon-only Dock display uses the full Dock commit pipeline");

    const auto newer = ParseGitHubRelease(
        R"({"tag_name":"v1.0.5.0","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/tag/v1.0.5.0"})",
        "1.0.4.0");
    Check(newer.parsed && newer.updateAvailable &&
            newer.version == "1.0.5.0",
        "a newer official GitHub release is detected");

    const auto current = ParseGitHubRelease(
        R"({"tag_name":"1.0.4.0","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/tag/v1.0.4.0"})",
        "1.0.4.0");
    Check(current.parsed && !current.updateAvailable,
        "the installed release is reported as current");

    const auto older = ParseGitHubRelease(
        R"({"tag_name":"v1.0.3.0","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/tag/v1.0.3.0"})",
        "1.0.4.0");
    Check(older.parsed && !older.updateAvailable,
        "an older official release never becomes an update");

    Check(!ParseGitHubRelease(
            R"({"tag_name":"v1.0.5.0","html_url":"https://example.com/download"})",
            "1.0.4.0").parsed,
        "an untrusted download URL is rejected");
    Check(!ParseGitHubRelease("not json", "1.0.4.0").parsed,
        "an invalid response is rejected");
    for (const char* invalidVersion : {
             "v0.1.0.0", "v65536.0.0.0", "v1.65536.0.0",
             "v1.0.65536.0", "v1.0.4.1", "v1.0.4",
             "v1.0.4.0.0", "v42949672960.0.0.0"})
    {
        const std::string body = std::string(
            R"({"tag_name":")") + invalidVersion +
            R"(","html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/latest"})";
        Check(!ParseGitHubRelease(body, "1.0.4.0").parsed,
            "versions outside the Store four-part contract are rejected");
    }
    Check(!ParseGitHubRelease(
            R"({"tag_name":"v1.0.5.0"})", "1.0.4.0").parsed,
        "a release without an official download URL is rejected");
    Check(!ParseGitHubRelease(
            R"({"html_url":"https://github.com/FreeFallingSnow/SnowDesktop_Release/releases/latest"})",
            "1.0.4.0").parsed,
        "a release without a version tag is rejected");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "Settings update rule checks passed\n";
    return EXIT_SUCCESS;
}
