#include "winui/settings_shell_navigation.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
using namespace snowdesktop;
using namespace snowdesktop::winui;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void TestHistoryAndFocusRoutes()
{
    SettingsShellNavigationState state;
    Check(state.Route().page == SettingsPage::General && !state.CanGoBack(),
        "navigation starts at the legacy General page without back history");

    Check(state.Navigate(SettingsRoute::ForPage(
              SettingsPage::General, "general.language")) &&
            state.Route().focusId == "general.language" &&
            state.CanGoBack(),
        "navigation preserves a stable focus target");
    Check(state.Navigate(SettingsRoute::ForWidget(
              L"clock-instance", "clock.time-zone")) &&
            state.Route().page == SettingsPage::WidgetSettings &&
            state.Route().widgetInstanceId == L"clock-instance",
        "widget routes preserve instance scope");

    const auto previous = state.GoBack();
    Check(previous && previous->page == SettingsPage::General,
        "back returns to the preceding settings page");
    Check(state.Navigate(SettingsRoute::ForPage(SettingsPage::Desktop)) &&
            !state.CanGoForward(),
        "a new branch discards forward history");
}

void TestConditionalPages()
{
    SettingsShellNavigationState state;
    Check(!state.Navigate(
              SettingsRoute::ForPage(SettingsPage::DeveloperTools)) &&
            !state.Navigate(SettingsRoute::ForPage(SettingsPage::Debug)),
        "developer and debug routes are hidden by default");

    Check(!state.SetVisibility({true, false}).has_value() &&
            state.Navigate(
                SettingsRoute::ForPage(SettingsPage::DeveloperTools)) &&
            !state.Navigate(SettingsRoute::ForPage(SettingsPage::Debug)),
        "the existing developer gate does not implicitly unlock Debug");

    const auto developerClosed = state.SetVisibility({false, true});
    Check(developerClosed && developerClosed->page == SettingsPage::General &&
            !state.Navigate(
                SettingsRoute::ForPage(SettingsPage::DeveloperTools)) &&
            state.Navigate(SettingsRoute::ForPage(SettingsPage::Debug)),
        "each conditional route requires its own runtime gate");

    const auto replacement = state.SetVisibility({false, false});
    Check(replacement && replacement->page == SettingsPage::General &&
            !state.CanGoBack(),
        "closing a conditional gate removes hidden history and returns General");
}

void TestControllerGenerationGate()
{
    SettingsShellNavigationState state;
    Check(state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::Personalization), 10, 3) &&
            state.Revision() == 10 && state.Generation() == 3,
        "first controller publication initializes revision and generation");

    Check(!state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::Desktop), 11, 2) &&
            state.Route().page == SettingsPage::Personalization,
        "an older generation cannot replace the visible route");
    Check(!state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::Desktop), 9, 3) &&
            !state.ApplyControllerUpdate(
                SettingsRoute::ForPage(SettingsPage::Desktop), 10, 3),
        "an older or duplicate revision in the current generation is stale");

    Check(state.Navigate(SettingsRoute::ForPage(SettingsPage::About)) &&
            state.CanGoBack(),
        "local history may grow within a controller generation");
    Check(state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::General), 1, 4) &&
            state.Route().page == SettingsPage::General &&
            !state.CanGoBack() && state.HistorySize() == 1,
        "a new generation clears history from the closed view session");
}

void TestControllerCommittedBackNavigation()
{
    SettingsShellNavigationState state;
    Check(state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::Home), 1, 7) &&
            state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::General), 2, 7) &&
            state.ApplyControllerUpdate(
              SettingsRoute::ForWidget(L"clock-instance"), 3, 7),
        "controller publications build route history");

    const auto target = state.PeekBack();
    Check(target && target->page == SettingsPage::General &&
            state.Route().page == SettingsPage::WidgetSettings,
        "back target can be inspected without changing presentation state");
    Check(target && state.ApplyControllerUpdate(*target, 4, 7) &&
            state.Route().page == SettingsPage::General &&
            state.CanGoForward() && state.HistorySize() == 3,
        "a validated controller back commit moves within existing history");
}

void TestInvalidRoutes()
{
    SettingsShellNavigationState state;
    Check(!state.Navigate(SettingsRoute::ForWidget(L"")),
        "an empty widget instance route is rejected");
    Check(!state.Navigate(SettingsRoute::ForPage(
              static_cast<SettingsPage>(0xff))),
        "an unknown settings page is rejected");
}

void TestGeneralPageSourceContract(const std::filesystem::path& root)
{
    const std::string shell = ReadText(
        root / "src/winui/SettingsShell.xaml.cpp");
    const std::string shellHeader = ReadText(
        root / "src/winui/SettingsShell.xaml.h");
    const std::string presenter = ReadText(
        root / "src/winui/general_page_presenter.cpp");
    const std::string presenterHeader = ReadText(
        root / "src/winui/general_page_presenter.h");
    const std::string personalization = ReadText(
        root / "src/winui/personalization_page_presenter.cpp");
    const std::string desktop = ReadText(
        root / "src/winui/desktop_page_presenter.cpp");
    const std::string dock = ReadText(
        root / "src/winui/dock_page_presenter.cpp");
    const std::string sharedControls = ReadText(
        root / "src/winui/settings_presenter_controls.h");
    const std::string shellXaml = ReadText(
        root / "src/winui/SettingsShell.xaml");
    const std::string recorder = ReadText(
        root / "src/winui/hotkey_recorder.cpp");
    const std::string recorderHeader = ReadText(
        root / "src/winui/hotkey_recorder.h");
    const std::string recorderRules = ReadText(
        root / "src/winui/hotkey_recorder_rules.h");

    Check(!shell.empty() && !shellHeader.empty() && !presenter.empty() &&
            !presenterHeader.empty() && !personalization.empty() &&
            !desktop.empty() && !dock.empty() &&
            !sharedControls.empty() && !shellXaml.empty() &&
            !recorder.empty() && !recorderHeader.empty() &&
            !recorderRules.empty(),
        "General page source-contract inputs are readable");

    Check(shell.find("addPlaceholder(\"general.startup\"") ==
                std::string::npos &&
            shell.find(
                "PageCards().Children().Append(generalPage_->Root())") !=
                std::string::npos,
        "General uses a real cached presenter instead of placeholder cards");
    Check(shell.find("if (!forcePageCards && renderedPageRoute_") !=
                std::string::npos &&
            shellHeader.find(
                "std::optional<snowdesktop::SettingsRoute> renderedPageRoute_") !=
                std::string::npos &&
            shell.find("generalPage_->ApplySnapshot(snapshot)") !=
                std::string::npos,
        "same-route controller revisions patch the cached page without rebuilding it");

    Check(presenter.find("snapshot.domainRevisions.general") !=
                std::string::npos &&
            presenter.find("snapshot.domainRevisions.navigation") !=
                std::string::npos &&
            presenter.find("snapshot.domainRevisions.dock") !=
                std::string::npos &&
            presenter.find("SettingsSnapshot snapshot_") ==
                std::string::npos &&
            presenter.find("SettingsValues values_") == std::string::npos,
        "General applies named domain revisions without retaining a snapshot copy");
    Check(presenter.find("updatingControls") != std::string::npos &&
            presenter.find("actions.commitGeneral(generation") !=
                std::string::npos &&
            presenter.find("actions.commitNavigation(generation") !=
                std::string::npos &&
            presenter.find("actions.commitDock(generation") !=
                std::string::npos,
        "control feedback is suppressed and edits are sent through latest-state actions");

    const auto desktopInteractionCard = presenter.find(
        "SetCardText(pageNavigationCard");
    const auto desktopInteractionTitle = presenter.find(
        "\"app.settings.desktop_interact\"", desktopInteractionCard);
    const auto pageNavigationRow = presenter.find(
        "pageNavigationToggleRow.SetText(", desktopInteractionTitle);
    Check(desktopInteractionCard != std::string::npos &&
            desktopInteractionTitle < pageNavigationRow,
        "Desktop Interaction remains the legacy group title before the page-navigation setting");

    const auto conditionalHintsStart = presenter.find(
        "void UpdateConditionalHintVisibility()");
    const auto conditionalHintsEnd = presenter.find(
        "void SelectLanguage()", conditionalHintsStart);
    const std::string_view conditionalHints =
        conditionalHintsStart != std::string::npos &&
                conditionalHintsEnd != std::string::npos
            ? std::string_view(presenter).substr(conditionalHintsStart,
                  conditionalHintsEnd - conditionalHintsStart)
            : std::string_view{};
    Check(conditionalHints.find(
              "desktopPassthroughToggleRow.help.Visibility(") !=
                std::string_view::npos &&
            conditionalHints.find("desktopPassthroughToggle.IsOn()") !=
                std::string_view::npos &&
            conditionalHints.find(
              "floatingDockToggleRow.help.Visibility(") !=
                std::string_view::npos &&
            conditionalHints.find("floatingDockToggle.IsOn()") !=
                std::string_view::npos &&
            conditionalHints.find("dockEnabled") == std::string_view::npos &&
            presenter.find(
              "void UpdateDependentEnabledStates()\n    {\n        UpdateConditionalHintVisibility();") !=
                std::string::npos &&
            presenter.find(
              "hasSnapshot = true;\n        UpdateDependentEnabledStates();") !=
                std::string::npos &&
            presenter.find(
              "RefreshStartupConflict();\n        UpdateConditionalHintVisibility();") !=
                std::string::npos,
        "desktop passthrough and floating-Dock hints follow only their own toggles and refresh after localization");
    Check(personalization.find(
              "FourThemeSelectionFromAppearancePreset(") !=
                std::string::npos &&
            personalization.find("settings.quickNavTheme = inheritedTheme") !=
                std::string::npos &&
            personalization.find(
              "settings.collectionPopupTheme = inheritedTheme") !=
                std::string::npos,
        "switching a built-in appearance to Custom carries its four-theme selection into the legacy quick-navigation surfaces");
    Check(presenter.find("autoStartToggle") != std::string::npos &&
            presenter.find("settings.autoStartEnabled = enabled") ==
                std::string::npos &&
            presenter.find("settings.autoStartEnabled") !=
                std::string::npos &&
            presenter.find("actions.setAutoStart(generation, enabled)") !=
                std::string::npos &&
            presenter.find("portableStartupConflict") !=
                std::string::npos &&
            presenter.find("installedStartupConflict") !=
                std::string::npos &&
            presenterHeader.find("openStartupAppsSettings") !=
                std::string::npos &&
            presenter.find("general.autoStart") != std::string::npos,
        "General projects host-owned startup state without committing it as JSON and preserves both ownership warnings");
    Check(presenter.find(
              "startupCard.content.Children().Append(portableStartupConflict)") <
                presenter.find(
              "startupCard.content.Children().Append(softwareDesktopRow.root)") &&
            presenter.find(
              "startupCard.content.Children().Append(installedStartupConflict)") <
                presenter.find(
              "startupCard.content.Children().Append(softwareDesktopRow.root)"),
        "startup ownership warnings remain immediately below Auto-start before Software Desktop");

    for (const char* target : {
             "HotkeyTarget::QuickNavigation",
             "HotkeyTarget::PagePrevious",
             "HotkeyTarget::PageNext",
             "HotkeyTarget::DesktopPassthrough",
             "HotkeyTarget::FloatingDock"})
    {
        Check(presenter.find(target) != std::string::npos,
            "each General shortcut has a typed HotkeyRecorder binding");
    }
    Check(presenter.find("TextBox") == std::string::npos &&
            recorder.find("TextBox") == std::string::npos &&
            recorderHeader.find("FocusTarget() const noexcept") !=
                std::string::npos &&
            recorder.find("return state_ ? state_->button : nullptr;") !=
                std::string::npos,
        "hotkeys use the dedicated recorder button and expose its focus target");
    Check(recorderHeader.find(
              "SetValidationContext(bool enabled, bool localDesktopHotkey)") !=
                std::string::npos &&
            recorder.find("ProbeCommittedAvailability(") !=
                std::string::npos &&
            recorder.find("target->committedRequestId != requestId") !=
                std::string::npos &&
            recorder.find("target->rules.Generation() != generation") !=
                std::string::npos &&
            recorder.find("!state->localDesktopHotkey") !=
                std::string::npos &&
            presenter.find(
              "recorder.SetValidationContext(enabled, localDesktopHotkey)") !=
                std::string::npos &&
            presenter.find("impl_->UpdateDependentEnabledStates();") !=
                std::string::npos,
        "saved hotkeys remain asynchronously validated with generation/request guards and local-desktop modifier semantics outside capture mode");
    for (const char* key : {
             "app.settings.hotkey_status_disabled",
             "app.settings.hotkey_not_set_warning",
             "app.settings.hotkey_status_conflict",
             "app.settings.hotkey_status_in_use",
             "app.settings.hotkey_conflict_system",
             "app.settings.hotkey_status_no_modifier",
             "app.settings.hotkey_no_modifier_warning",
             "app.settings.hotkey_status_available",
             "app.settings.hotkey_available"})
    {
        Check(presenter.find(key) != std::string::npos,
            "every saved-hotkey status uses an existing localized key");
    }
    Check(shell.find("generalPage_->Deactivate()") != std::string::npos &&
            presenter.find("void GeneralPagePresenter::Deactivate()") !=
                std::string::npos,
        "leaving or closing General cancels any active recorder");

    Check(shell.find(
              "widgetsPage_->DeveloperToolsContent()") !=
                std::string::npos &&
            shell.find("homeAboutPage_->DebugContent()") !=
                std::string::npos &&
            shell.find(
              "addPlaceholder(\"developer.overrides\"") ==
                std::string::npos &&
            shell.find("addPlaceholder(\"debug.runtime\"") ==
                std::string::npos &&
             shell.find("std::erase_if(results") != std::string::npos &&
             shell.find("navigation_.IsRouteAvailable(result.route)") !=
                 std::string::npos &&
             shell.find("searchItems_.GetAt(") != std::string::npos &&
             shell.find("SearchDisplayText(result) == selectedText") ==
                 std::string::npos,
        "conditional routes use their parity presenters and hidden search results are filtered at the shell boundary");

    Check(shell.find("dockPage_->DockEnableContent()") !=
                std::string::npos &&
            shell.find("generalPage_->DockShortcutContent()") !=
                std::string::npos &&
            shell.find("dockPage_->DockContent()") !=
                std::string::npos &&
            shell.find("desktopPage_->AppearanceContent()") !=
                std::string::npos &&
            shell.find("dockPage_->TaskbarContent()") !=
                std::string::npos &&
            shell.find("desktopPage_->CategoryContent()") !=
                std::string::npos &&
            shell.find("result.route.page = result.focusId.starts_with") !=
                std::string::npos,
        "legacy General, Appearance, and Category ownership is composed from single-parent presenter sections and search follows the moved controls");

    const auto generalItem = shellXaml.find("x:Name=\"GeneralItem\"");
    const auto homeItem = shellXaml.find("x:Name=\"HomeItem\"");
    const auto appearanceItem = shellXaml.find(
        "x:Name=\"PersonalizationItem\"");
    const auto categoryItem = shellXaml.find("x:Name=\"DesktopItem\"");
    const auto widgetsItem = shellXaml.find("x:Name=\"WidgetsItem\"");
    const auto developerItem = shellXaml.find("x:Name=\"DeveloperItem\"");
    const auto backupItem = shellXaml.find("x:Name=\"BackupItem\"");
    const auto aboutItem = shellXaml.find("x:Name=\"AboutItem\"");
    const auto debugItem = shellXaml.find("x:Name=\"DebugItem\"");
    const auto homeCollapsed = shellXaml.find(
        "Visibility=\"Collapsed\"", homeItem);
    const auto dockItem = shellXaml.find("x:Name=\"DockItem\"");
    const auto dockCollapsed = shellXaml.find(
        "Visibility=\"Collapsed\"", dockItem);
    Check(generalItem < appearanceItem && appearanceItem < categoryItem &&
            categoryItem < widgetsItem && widgetsItem < developerItem &&
            developerItem < backupItem && backupItem < aboutItem &&
            aboutItem < debugItem && homeItem < homeCollapsed &&
            homeCollapsed < generalItem && dockItem < dockCollapsed &&
            dockCollapsed < widgetsItem &&
            shellXaml.find("NavigationView.FooterMenuItems") ==
                std::string::npos,
        "the visible navigation declaration retains the legacy ordering while Home and Dock remain compatibility-only items");

    const auto dismissStart = sharedControls.find("void Dismiss() noexcept");
    const auto dismissEnd = sharedControls.find("void Close() noexcept",
        dismissStart);
    const auto dismissRollback = sharedControls.find("Rollback();",
        dismissStart);
    const auto dismissHide = sharedControls.find("flyout.Hide();",
        dismissStart);
    const auto cancelStart = sharedControls.find(
        "cancelToken = cancel.Click");
    const auto cancelEnd = sharedControls.find(
        "closedToken = flyout.Closed", cancelStart);
    const auto cancelRollback = sharedControls.find("Rollback();", cancelStart);
    const auto cancelHide = sharedControls.find("flyout.Hide();", cancelStart);
    Check(sharedControls.find("struct SettingRow") != std::string::npos &&
            sharedControls.find("kSettingControlWidth = 520.0") !=
                std::string::npos &&
            sharedControls.find("muxc::Grid::SetColumn(controlHost, 1)") !=
                std::string::npos &&
            sharedControls.find("void SetControlAlignment(") !=
                std::string::npos &&
            sharedControls.find("kStackedSettingThreshold = 760.0") !=
                std::string::npos &&
            sharedControls.find("root.SizeChanged(") != std::string::npos &&
            sharedControls.find(
                "Grid::SetRow(currentControl, stacked ? 1 : 0)") !=
                std::string::npos &&
            sharedControls.find("struct ColorFlyoutEditor") !=
                std::string::npos &&
            sharedControls.find("SettingsUpdateMode::Preview") !=
                std::string::npos &&
            sharedControls.find("SettingsUpdateMode::PreviewAndCommit") !=
                std::string::npos &&
            sharedControls.find("Rollback()") != std::string::npos &&
            dismissStart != std::string::npos &&
            dismissRollback < dismissHide && dismissHide < dismissEnd &&
            cancelStart != std::string::npos &&
            cancelRollback < cancelHide && cancelHide < cancelEnd &&
            personalization.find("mux::DispatcherTimer") !=
                std::string::npos &&
            personalization.find("std::chrono::milliseconds(650)") !=
                std::string::npos &&
            personalization.find("SetUnit(widgetAlpha, L\"%\")") !=
                std::string::npos &&
            personalization.find("SetUnit(blurRadius, L\"px\")") !=
                std::string::npos &&
            personalization.find("SetUnit(cornerRadius, L\"cu\")") !=
                std::string::npos &&
            personalization.find("backgroundColor.editor.button") !=
                std::string::npos &&
            desktop.find("backgroundStart->editor.button") !=
                std::string::npos &&
            dock.find("taskbarBackgroundColor.editor.button") !=
                std::string::npos,
        "setting rows keep wide left-right alignment, stack below the threshold, and color swatches transactionally restore unconfirmed sessions");

    const auto firstFontReset = desktop.find("}, 16.0);");
    const auto numericResetPublish = desktop.find("changed(defaultValue,");
    const auto numericResetCommit = desktop.find(
        "SettingsUpdateMode::PreviewAndCommit", numericResetPublish);
    const auto numericResetEnd = desktop.find(");", numericResetPublish);
    Check(presenter.find(
              "settings.modifiers = MOD_CONTROL | MOD_ALT") !=
                std::string::npos &&
            presenter.find("settings.virtualKey = VK_SPACE") !=
                std::string::npos &&
            presenter.find(
              "settings.pageNavigationPreviousVirtualKey = VK_PRIOR") !=
                std::string::npos &&
            presenter.find(
              "settings.pageNavigationNextVirtualKey = VK_NEXT") !=
                std::string::npos &&
            presenter.find(
              "settings.desktopPassthroughHotkeyVirtualKey =") !=
                std::string::npos &&
            presenter.find("VK_OEM_3;") != std::string::npos &&
            presenter.find("floatingHotkeyVirtualKey = 'D'") !=
                std::string::npos &&
            presenter.find("actions.Children().Append(reset)") !=
                std::string::npos &&
            presenter.find(
              "reset.VerticalAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos &&
            presenter.find(
              "autoStartRow.SetControlAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            presenter.find("quickNavigationToggleRow.SetControlAlignment(") !=
                std::string::npos &&
            presenter.find("pageNavigationToggleRow.SetControlAlignment(") !=
                std::string::npos &&
            presenter.find("desktopPassthroughToggleRow.SetControlAlignment(") !=
                std::string::npos &&
            presenter.find("floatingDockToggleRow.SetControlAlignment(") !=
                std::string::npos &&
            recorderRules.find(
              "virtualKey == KeyBack || virtualKey == KeyDelete") !=
                std::string::npos &&
            recorderRules.find("committed_ = {};") != std::string::npos &&
            recorderRules.find("HotkeyRecorderAction::Clear") !=
                std::string::npos &&
            desktop.find(
              "constexpr double kDefaultIconSpacingScale = 1.0") !=
                std::string::npos &&
            desktop.find(
              "}, kDefaultIconSpacingScale * 100.0);") !=
                std::string::npos &&
            desktop.find(
              "}, kDefaultItemIconSizeScale * 100.0);") !=
                std::string::npos &&
            firstFontReset != std::string::npos &&
            desktop.find("}, 16.0);", firstFontReset + 1) !=
                std::string::npos &&
            desktop.find("}, 600.0);") != std::string::npos &&
            numericResetPublish != std::string::npos &&
            numericResetCommit != std::string::npos &&
            numericResetEnd != std::string::npos &&
            numericResetCommit < numericResetEnd &&
            personalization.find("1.0, 12.0") != std::string::npos &&
            personalization.find("1.0, 24.0") != std::string::npos &&
            personalization.find("1.0, 34.0") != std::string::npos &&
            dock.find("nullptr, 100.0") != std::string::npos,
        "legacy hotkey reset/clear paths and every numeric reset default remain available beside their editors");
}
}

int main(int argc, char** argv)
{
    TestHistoryAndFocusRoutes();
    TestConditionalPages();
    TestControllerGenerationGate();
    TestControllerCommittedBackNavigation();
    TestInvalidRoutes();
    Check(argc == 2,
        "source root is supplied for WinUI settings source contracts");
    if (argc == 2)
        TestGeneralPageSourceContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures << " WinUI settings navigation check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI settings navigation checks passed\n";
    return EXIT_SUCCESS;
}
