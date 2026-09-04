#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string source = contents.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
}

std::string_view Between(std::string_view source,
    std::string_view beginMarker, std::string_view endMarker)
{
    const std::size_t begin = source.find(beginMarker);
    if (begin == std::string_view::npos)
        return {};
    const std::size_t end = source.find(
        endMarker, begin + beginMarker.size());
    if (end == std::string_view::npos)
        return source.substr(begin);
    return source.substr(begin, end - begin);
}

bool AppearsBefore(std::string_view source,
    std::string_view first, std::string_view second)
{
    const std::size_t firstPosition = source.find(first);
    const std::size_t secondPosition = source.find(second);
    return firstPosition != std::string_view::npos &&
        secondPosition != std::string_view::npos &&
        firstPosition < secondPosition;
}
} // namespace

int main(int argc, char** argv)
{
    Check(argc == 2, "source root argument is provided");
    if (argc != 2)
        return 1;

    const std::filesystem::path root(argv[1]);
    const std::string source = ReadFile(
        root / "src" / "app" / "app_settings_apply.cpp");
    const std::string pageGrid = ReadFile(
        root / "src" / "app" / "app_page_grid.cpp");
    const std::string run = ReadFile(
        root / "src" / "app" / "app_run.cpp");
    const std::string controllerHeader = ReadFile(
        root / "src" / "settings_controller.h");
    const std::string hostHeader = ReadFile(
        root / "src" / "winui" / "settings_window_host.h");
    const std::string host = ReadFile(
        root / "src" / "winui" / "settings_window_host.cpp");
    Check(!source.empty() && !pageGrid.empty() && !run.empty() &&
            !controllerHeader.empty() &&
            !hostHeader.empty() && !host.empty(),
        "settings host action sources are readable");

    const std::string_view layoutProjection = Between(run,
        "LoadLayoutSlots();", "UpdateLayoutWorkArea();");
    Check(AppearsBefore(layoutProjection,
            "SynchronizeGeneral(generalSettings_)",
            "SynchronizeDesktop(") &&
            layoutProjection.find(
                "desktopSettings.dockEnabled = generalSettings_.dockEnabled;") !=
                std::string_view::npos,
        "layout-owned Dock enablement reaches General and Desktop snapshots before WinUI observes them");

    const std::string_view preview = Between(source,
        "snowdesktop::SettingsActionResult OnSettingsPreview(",
        "snowdesktop::SettingsActionResult OnSettingsCommitted(");
    const std::string_view commit = Between(source,
        "snowdesktop::SettingsActionResult OnSettingsCommitted(",
        "snowdesktop::SettingsActionResult OnSettingsRouteChanged(");
    Check(!preview.empty() && !commit.empty(),
        "settings host preview and commit handlers are discoverable");

    Check(AppearsBefore(preview,
            "const bool committedTaskbarAutoHide",
            "app_.dockSettings_ = snapshot.values.dock;") &&
            AppearsBefore(preview,
                "app_.dockSettings_ = snapshot.values.dock;",
                "app_.dockSettings_.systemTaskbarAutoHide =") &&
            AppearsBefore(preview,
                "app_.dockSettings_ = snapshot.values.dock;",
                "app_.dockSettings_.systemTaskbarAlignment ="),
        "Dock previews retain the committed system taskbar mirrors");
    Check(preview.find("RequestSystemTaskbar") == std::string_view::npos,
        "Dock previews never request Windows-owned taskbar changes");
    Check(preview.find("app_.RefreshSystemTaskbarAppearance(false);") !=
                std::string_view::npos,
        "Dock appearance sliders repaint the system taskbar during preview");
    Check(preview.find(
              "snowdesktop::IconBeautifyUpdateKind::Preview") !=
                std::string_view::npos &&
            preview.find("app_.SetIconBeautifySettings(") !=
                std::string_view::npos,
        "desktop icon beautification colors and sliders reach the live preview path");
    Check(preview.find("app_.PreviewItemFontSize(") !=
                std::string_view::npos &&
            preview.find("app_.PreviewListItemFontSize(") !=
                std::string_view::npos &&
            preview.find("app_.PreviewItemFontWeight(") !=
                std::string_view::npos,
        "desktop typography sliders repaint without persisting each drag step");

    const std::string_view iconBeautifyUpdate = Between(pageGrid,
        "void DesktopApp::SetIconBeautifySettings(",
        "size_t DesktopApp::FirstMonitorOrderIndex() const");
    Check(iconBeautifyUpdate.find("InvalidateDockRects(TRUE);") !=
                std::string_view::npos,
        "icon beautification changes invalidate embedded and persistent Dock surfaces");
    Check(iconBeautifyUpdate.find(
              "snowdesktop::IconBeautifyUpdateKind::Preview") !=
                std::string_view::npos &&
            iconBeautifyUpdate.find("PresentDesktopPointerUpdate();") !=
                std::string_view::npos &&
            iconBeautifyUpdate.find("FlushPendingCompositionCommit();") !=
                std::string_view::npos,
        "icon beautification previews present without waiting for desktop pointer input");

    const std::size_t dockAssignment = commit.find(
        "app_.dockSettings_ = requestedDockSettings;");
    const std::size_t autoHideRequest = commit.find(
        "RequestSystemTaskbarAutoHideEnabled(");
    const std::size_t alignmentRequest = commit.find(
        "RequestSystemTaskbarAlignmentCentered(");
    Check(autoHideRequest != std::string_view::npos &&
            alignmentRequest != std::string_view::npos &&
            dockAssignment != std::string_view::npos &&
            autoHideRequest < dockAssignment &&
            alignmentRequest < dockAssignment,
        "system taskbar requests precede mutation of the application mirror");
    Check(commit.find("if (autoHideChanged &&") !=
                std::string_view::npos &&
            commit.find("if (alignmentChanged &&") !=
                std::string_view::npos,
        "system taskbar requests are issued only for changed values");
    Check(commit.find(
            "_LW(\"settings.taskbar.autoHide.queueFailed\")") !=
                std::string_view::npos &&
            commit.find(
                "_LW(\"settings.taskbar.alignment.queueFailed\")") !=
                std::string_view::npos &&
            commit.find("SettingsDomain::Dock);") !=
                std::string_view::npos,
        "rejected system taskbar requests return localized explicit Dock-domain failures");
    Check(commit.find("SyncSystemTaskbarSettingsFromWindows();") ==
            std::string_view::npos,
        "a successful Dock commit keeps requested values instead of rereading Windows");

    const std::string_view routeChanged = Between(source,
        "snowdesktop::SettingsActionResult OnSettingsRouteChanged(",
        "snowdesktop::SettingsActionResult Invoke(");
    Check(routeChanged.find(
              "route.page == snowdesktop::SettingsPage::Taskbar") !=
                std::string_view::npos &&
            routeChanged.find(
              "app_.SyncSystemTaskbarSettingsFromWindows();") !=
                std::string_view::npos &&
            routeChanged.find("RequestSystemTaskbar") ==
                std::string_view::npos,
        "entering the Taskbar route reconciles Windows-owned state without issuing a write request");

    const std::string_view taskbarSync = Between(source,
        "void DesktopApp::SyncSystemTaskbarSettingsFromWindows()",
        "void DesktopApp::LoadCategorySettingsAndApply()");
    Check(taskbarSync.find("snapshot->externalReplacementPending") !=
                std::string_view::npos &&
            taskbarSync.find("snapshot->dirtyDomains") !=
                std::string_view::npos &&
            AppearsBefore(taskbarSync,
                "settingsController_->SynchronizeSystemTaskbarState(",
                "dockSettings_.systemTaskbarAutoHide = autoHide;") &&
            taskbarSync.find(
              "if (persistedMirrorChanged && !dockDraftPending)") !=
                std::string_view::npos,
        "external taskbar reconciliation updates only Windows-owned fields and never persists unrelated Dock drafts");

    const std::string_view externalRefresh = Between(run,
        "settingsHostOptions.refreshExternalState = [this]()",
        "settingsHostOptions.developerToolsVisible = [this]()");
    Check(externalRefresh.find(
              "SyncSystemTaskbarSettingsFromWindows();") !=
                std::string_view::npos &&
            externalRefresh.find("IsSystemTaskbarAutoHideEnabled()") ==
                std::string_view::npos,
        "window reopen and in-window Taskbar navigation share one external-state reconciliation path");

    const std::string_view generalReload = Between(source,
        "void DesktopApp::LoadGeneralSettingsAndApply()",
        "void DesktopApp::ApplyQuickNavigationAppearance()");
    Check(AppearsBefore(generalReload,
              "const bool autoStartEnabled = generalSettings_.autoStartEnabled;",
              "LoadGeneralSettings(") &&
            AppearsBefore(generalReload,
              "generalSettings_ = settings;",
              "generalSettings_.autoStartEnabled = autoStartEnabled;"),
        "persisted General reloads preserve the Windows-owned auto-start projection");

    const std::string_view general = Between(commit,
        "if (HasSettingsDomain(domains, SettingsDomain::General))",
        "if (HasSettingsDomain(domains, SettingsDomain::Category))");
    Check(!general.empty(), "General commit block is discoverable");
    Check(AppearsBefore(general,
            "const bool dockEnabledChanged",
            "app_.generalSettings_ = snapshot.values.general;") &&
            general.find("if (dockEnabledChanged)") !=
                std::string_view::npos,
        "General commits detect Dock enablement changes before replacing state");
    Check(general.find(
            "desktop.dockEnabled =\n                        app_.generalSettings_.dockEnabled;") !=
                std::string_view::npos &&
            general.find("SynchronizeDesktop(") != std::string_view::npos,
        "General Dock enablement keeps the controller desktop mirror aligned");
    Check(AppearsBefore(general,
            "app_.UpdateLayoutWorkArea();",
            "app_.RestoreDockEntriesToDesktop();") &&
            AppearsBefore(general,
                "app_.RestoreDockEntriesToDesktop();",
                "app_.LayoutItems();") &&
            AppearsBefore(general,
                "app_.LayoutItems();",
                "app_.SaveLayoutSlots();") &&
            AppearsBefore(general,
                "app_.SaveLayoutSlots();",
                "app_.InvalidateDragStaticScene();"),
        "General Dock enablement changes relayout and persist in runtime order");
    Check(general.find("if (!app_.generalSettings_.dockEnabled)\n") !=
                std::string_view::npos,
        "disabling the Dock restores its entries to the desktop before saving");

    Check(general.find("ApplyAutoStartEnabled") == std::string_view::npos,
        "General JSON commits never mutate the Windows auto-start registration");
    Check(controllerHeader.find("SetAutoStartEnabled") !=
                std::string::npos &&
            controllerHeader.find("OpenStartupAppsSettings") ==
                std::string::npos &&
            controllerHeader.find("bool boolValue = false;") !=
                std::string::npos,
        "auto-start uses one explicit host action without delegating state to Windows Startup Apps");
    const std::string_view autoStartAction = Between(source,
        "case Action::SetAutoStartEnabled:",
        "case Action::CheckForUpdates:");
    Check(!autoStartAction.empty() &&
            AppearsBefore(autoStartAction,
                "app_.ApplyAutoStartEnabled(request.boolValue)",
                "SynchronizeGeneral(") &&
            autoStartAction.find("result.state.stateKnown &&") !=
                std::string_view::npos,
        "auto-start publishes the authoritative Windows result even after a rejected request");
    Check(hostHeader.find("startupConflict") != std::string::npos &&
            hostHeader.find("advancedFeatureStatus") !=
                std::string::npos &&
            hostHeader.find("registerAdvancedFeatures") !=
                std::string::npos &&
            host.find("general.setAutoStart") != std::string::npos &&
            host.find("general.openAdvancedFeaturesStore") !=
                std::string::npos &&
            host.find("SnowDesktopSteamStoreUrl()") !=
                std::string::npos &&
            host.find("general.openStartupAppsSettings") ==
                std::string::npos &&
            host.find("general.queryStartupConflict") !=
                std::string::npos &&
            host.find("Action::SetAutoStartEnabled") !=
                std::string::npos &&
            host.find("Action::OpenStartupAppsSettings") ==
                std::string::npos,
        "the General WinUI presenter owns auto-start changes and receives runtime startup and Steam entitlement state");
    Check(run.find("target.cardVisible = source.bridgeAvailable") !=
                std::string::npos &&
            run.find("RuntimeDeploymentKind::Portable") !=
                std::string::npos &&
            run.find("target.offerSteamStore = !source.bridgeAvailable") !=
                std::string::npos &&
            host.find("descriptor.focusId == \"general.advancedFeatures\"") !=
                std::string::npos,
        "only portable Bridge-free builds offer the Steam Store while hidden advanced-feature cards are excluded from Settings search");
    Check(run.find("settingsHostOptions.startupConflict") !=
                std::string::npos &&
            run.find("QueryAutoStartState()") != std::string::npos &&
            run.find("ClassifyAutoStartOwnershipNotice") !=
                std::string::npos &&
            run.find("state.packaged && otherOwnerActive") ==
                std::string::npos &&
            run.find("NonPackagedVersionOwnsStartup") !=
                std::string::npos &&
            run.find("InstalledVersionOwnsStartup") != std::string::npos,
        "DesktopApp projects startup ownership by the active task owner so Steam detects portable and stale Steam targets");
    Check(host.find("personalization.updateGeneral") !=
                std::string::npos &&
            host.find("actions.setDeveloperToolsEnabled") !=
                std::string::npos &&
            run.find("snapshot->values.general.") !=
                std::string::npos,
        "legacy General-owned appearance choices and the developer-tools switch use the current controller snapshot");
    Check(controllerHeader.find("SetAnimationDiagnostics") !=
                std::string::npos &&
            controllerHeader.find("TriggerCrashTest") !=
                std::string::npos &&
            host.find("homeAbout.openLink") != std::string::npos &&
            host.find("HomeAboutLinkUri(link)") != std::string::npos &&
            host.find("settings.about.link.openFailed") !=
                std::string::npos &&
            host.find("homeAbout.setAnimationDiagnostics") !=
                std::string::npos &&
            host.find("Action::SetAnimationDiagnostics") !=
                std::string::npos &&
            host.find("homeAbout.unlockDebug") != std::string::npos &&
            host.find("debugUnlocked = true") != std::string::npos &&
            host.find("RebuildSearchIndex();") != std::string::npos &&
            host.find("homeAbout.requestCrashTestConfirmation") !=
                std::string::npos &&
            host.find("ShowGenerationConfirmation(") !=
                std::string::npos &&
            host.find("Action::TriggerCrashTest") !=
                std::string::npos,
        "About links and legacy Debug controls use localized generation-gated host actions and confirmation");
    Check(run.find("widgetsPage.agentSkillTargetMask") ==
                std::string::npos &&
            run.find("widgetsPage.setAgentSkillTargetMask") ==
                std::string::npos &&
            run.find("widgetsPage.openDevelopmentFolder") !=
                std::string::npos &&
            run.find("WidgetEngine::GetWidgetPackagePaths()") !=
                std::string::npos,
        "Developer Tools does not persist detected Agent Skill installation scope and opens the authoritative workspace");
    Check(run.find("WidgetSettingsService searchReader(") !=
                std::string::npos &&
            run.find("searchReader.Load(widget.id)") !=
                std::string::npos &&
            run.find("fieldState.visible") != std::string::npos,
        "settings search evaluates visible v2 fields without mutating live widget-settings sessions");
    const std::string_view languageChange = Between(source,
        "void DesktopApp::ApplyLanguageChange()",
        "void DesktopApp::ToggleDesktopIconsVisibility()");
    Check(AppearsBefore(languageChange,
              "settingsWindow_->PrepareLanguageChange()",
              "widgetEngine_->ReloadWidget(widget.id)") &&
            AppearsBefore(languageChange,
              "widgetEngine_->ReloadWidget(widget.id)",
              "settingsWindow_->ApplyLanguageChange(") &&
            languageChange.find("widgetRuntimeReloadAllowed") !=
                std::string_view::npos,
        "component language changes preserve pending edits, reload runtimes, then rebuild the WinUI schema and search index");
    const std::string_view hotkeyProbe = Between(source,
        "HotkeyProbeResult ProbeHotkeyAvailability(", "DesktopApp& app_");
    const auto reservedPageKey =
        hotkeyProbe.find("IsReservedDesktopSingleKey(");
    const auto internalPageAvailable = hotkeyProbe.find(
        "if (target == HotkeyTarget::PagePrevious ||\n"
        "            target == HotkeyTarget::PageNext)",
        reservedPageKey);
    const auto systemHotkeyProbe = hotkeyProbe.find("RegisterHotKey(");
    Check(hotkeyProbe.find("conflictsWith(HotkeyTarget::PagePrevious") !=
                std::string_view::npos &&
            hotkeyProbe.find("conflictsWith(HotkeyTarget::PageNext") !=
                std::string_view::npos &&
            hotkeyProbe.find(
                "return { false, HotkeyTarget::QuickNavigation }") !=
                std::string_view::npos &&
            hotkeyProbe.find(
                "return { false, HotkeyTarget::DesktopPassthrough }") !=
                std::string_view::npos &&
            hotkeyProbe.find(
                "return { false, HotkeyTarget::FloatingDock }") !=
                std::string_view::npos &&
            hotkeyProbe.find("IsReservedDesktopSingleKey(") !=
                std::string_view::npos &&
            internalPageAvailable != std::string_view::npos &&
            hotkeyProbe.find("return { true, HotkeyTarget::None };",
                internalPageAvailable) < systemHotkeyProbe &&
            reservedPageKey < internalPageAvailable &&
            internalPageAvailable < systemHotkeyProbe,
        "page-navigation hotkeys become available after reserved/internal conflict checks without entering the RegisterHotKey probe");
    Check(source.find("HotkeyTargetLabelKey(") != std::string::npos &&
            source.find("_LFW(\"app.settings.hotkey_conflict_with\"") !=
                std::string::npos &&
            source.find("_LW(\"app.settings.hotkey_status_conflict\"") !=
                std::string::npos &&
            source.find("_LW(\"app.settings.hotkey_status_in_use\"") !=
                std::string::npos &&
            source.find("_LW(\"app.settings.hotkey_conflict_system\"") !=
                std::string::npos &&
            source.find("The hotkey is unavailable.") ==
                std::string::npos &&
            source.find("Hotkey probes require a typed capture target.") ==
                std::string::npos &&
            host.find(
                "completion(result.Succeeded(), result.message)") !=
                std::string::npos,
        "hotkey probe failures cross the host boundary as existing localized status/detail text instead of generic raw English");

    if (failures == 0)
        std::cout << "Settings host action semantics tests passed\n";
    return failures == 0 ? 0 : 1;
}
