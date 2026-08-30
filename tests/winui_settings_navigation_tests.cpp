#include "winui/settings_shell_navigation.h"

#include <algorithm>
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
    std::string source = content.str();
    source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
    return source;
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
    Check(state.Navigate(SettingsRoute::ForPage(
              SettingsPage::AppearanceWidgets,
              "personalization.cornerRadius")) &&
            state.Route().page == SettingsPage::AppearanceWidgets &&
            state.Route().focusId == "personalization.cornerRadius",
        "Appearance leaves are available as first-class navigation targets");
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
    Check(developerClosed && developerClosed->page == SettingsPage::Widgets &&
            !state.Navigate(
                SettingsRoute::ForPage(SettingsPage::DeveloperTools)) &&
            state.Navigate(SettingsRoute::ForPage(SettingsPage::Debug)),
        "closing Developer Tools returns to its legacy Widgets parent");

    const auto replacement = state.SetVisibility({false, false});
    Check(replacement && replacement->page == SettingsPage::About &&
            !state.CanGoBack(),
        "closing Debug removes hidden history and returns to About");
}

void TestControllerGenerationGate()
{
    SettingsShellNavigationState state;
    Check(state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::Personalization), 10, 3) &&
            state.Revision() == 10 && state.Generation() == 3 &&
            state.Route().page == SettingsPage::AppearanceTheme,
        "first controller publication canonicalizes compatibility input and initializes revision and generation");

    Check(!state.ApplyControllerUpdate(
              SettingsRoute::ForPage(SettingsPage::Desktop), 11, 2) &&
            state.Route().page == SettingsPage::AppearanceTheme,
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
    const std::string pages = ReadText(
        root / "src/winui/page_layout_page_presenter.cpp");
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
            !desktop.empty() && !dock.empty() && !pages.empty() &&
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
    const std::size_t shellConstructorStart = shell.find(
        "SettingsShell::SettingsShell()");
    const std::size_t shellConstructorEnd = shell.find(
        "void SettingsShell::EnsurePresentersForPage(",
        shellConstructorStart);
    const std::string shellConstructor =
        shellConstructorStart == std::string::npos ||
            shellConstructorEnd == std::string::npos
        ? std::string{}
        : shell.substr(shellConstructorStart,
              shellConstructorEnd - shellConstructorStart);
    Check(shellConstructor.find("make_unique") == std::string::npos &&
            shell.find("void SettingsShell::EnsurePresentersForPage(") !=
                std::string::npos &&
            shell.find("if (!sessionActive_)") != std::string::npos &&
            shell.find("EnsureWidgetSettingsPresenter()") !=
                std::string::npos &&
            shellHeader.find("generalPageActions_") != std::string::npos &&
            shellHeader.find("widgetsPageActions_") != std::string::npos,
        "inactive settings sessions keep page trees uncreated and lazily bind saved actions for the active route");

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
        "\"settings.general.pageNavigation\"", desktopInteractionCard);
    const auto pageNavigationRow = presenter.find(
        "pageNavigationToggleRow.SetText(", desktopInteractionTitle);
    Check(desktopInteractionCard != std::string::npos &&
            desktopInteractionTitle < pageNavigationRow,
        "Desktop page navigation has a task-specific group title before its controls");
    Check(presenter.find(
              "SetCardText(desktopPassthroughCard,\n            \"settings.desktop.passthrough\"") !=
                std::string::npos &&
            presenter.find(
              "SetCardText(floatingDockCard,\n            \"settings.dock.floatingShortcut\"") !=
                std::string::npos,
        "Desktop passthrough and the floating Dock shortcut avoid repeating their first setting as the card title");

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
            presenter.find("startupOwnershipNotice") !=
                std::string::npos &&
            presenter.find(
              "startupOwnershipNotice.Visibility(showNotice") !=
                std::string::npos &&
            presenter.find("InfoBarSeverity::Informational") !=
                std::string::npos &&
            presenterHeader.find("openStartupAppsSettings") ==
                std::string::npos &&
            presenter.find("auto_start_open_windows_settings") ==
                std::string::npos &&
            presenter.find("general.autoStart") != std::string::npos,
        "General projects the app-owned startup task without committing it as JSON or linking to Windows' independent startup state");
    Check(presenterHeader.find("RefreshRuntimeState() noexcept") !=
                std::string::npos &&
            presenter.find(
              "void GeneralPagePresenter::RefreshRuntimeState() noexcept") !=
                std::string::npos &&
            presenter.find("impl_->RefreshStartupConflict();") !=
                std::string::npos,
        "cached General pages expose an explicit runtime-state refresh for startup conflicts");
    Check(presenter.find(
              "startupCard.content.Children().Append(startupOwnershipNotice)") !=
                std::string::npos &&
            presenter.find(
              "InitializeCard(desktopBehaviorCard, cardStyle, desktopRoot)") !=
                std::string::npos &&
            presenter.find(
              "desktopBehaviorCard.content.Children().Append(softwareDesktopRow.root)") !=
                std::string::npos &&
            presenter.find(
              "desktopBehaviorCard.content.Children().Append(doubleClickHideRow.root)") !=
                std::string::npos &&
            presenter.find(
              "InitializeCard(desktopPassthroughCard, cardStyle, desktopRoot)") !=
                std::string::npos &&
            presenterHeader.find("DesktopBehaviorContent()") !=
                std::string::npos,
        "startup ownership notices stay with Auto-start while desktop behavior and passthrough have a dedicated Desktop surface");

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
                std::string::npos &&
            recorder.find("CreateChordVisual(") != std::string::npos &&
            recorder.find("CreateKeycap(") != std::string::npos &&
            recorder.find("depth.Padding({0.0, 0.0, 0.0, 2.0})") !=
                std::string::npos &&
            recorder.find(
              "Opacity(IsHighContrastEnabled() ? 1.0 : 0.72)") !=
                std::string::npos &&
            recorder.find(
              "Opacity(IsHighContrastEnabled() ? 1.0 : 0.68)") !=
                std::string::npos &&
            recorder.find("SetItemStatus(") != std::string::npos &&
            recorder.find("CreateEditButtonContent(") !=
                std::string::npos &&
            recorder.find("state->captureSurface.Content(") !=
                std::string::npos &&
            recorder.find("state->dialogStatus.Text(dialogStatus)") !=
                std::string::npos &&
            recorder.find("ActualThemeChanged(") != std::string::npos &&
            recorder.find("IsHighContrastEnabled()") !=
                std::string::npos,
        "hotkeys use a compact edit button and dialog recording surface with layered keycaps, theme and High Contrast updates, accessible chord status, and a stable focus target");
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

    const auto renderCards = shell.find(
        "void SettingsShell::RenderPageCards(");
    const auto personalizationCase = shell.find(
        "case SettingsPage::AppearanceTheme:", renderCards);
    const auto widgetAppearanceCase = shell.find(
        "case SettingsPage::AppearanceWidgets:", personalizationCase);
    const auto desktopCase = shell.find(
        "case SettingsPage::Desktop:", widgetAppearanceCase);
    const auto pagesCase = shell.find(
        "case SettingsPage::DesktopPages:", desktopCase);
    const auto desktopIconsCase = shell.find(
        "case SettingsPage::AppearanceDesktopIcons:", pagesCase);
    const auto beautificationCase = shell.find(
        "case SettingsPage::AppearanceIconBeautification:",
        desktopIconsCase);
    const auto categoriesCase = shell.find(
        "case SettingsPage::DesktopCategories:", beautificationCase);
    const auto dockCase = shell.find(
        "case SettingsPage::Dock:", categoriesCase);
    const auto taskbarCase = shell.find(
        "case SettingsPage::Taskbar:", dockCase);
    const auto compatibilityCase = shell.find(
        "case SettingsPage::DockAndTaskbar:", taskbarCase);
    const auto section = [&](std::size_t begin, std::size_t end) {
        return begin != std::string::npos && end != std::string::npos
            ? std::string_view(shell).substr(begin, end - begin)
            : std::string_view{};
    };
    const auto personalizationSection = section(
        personalizationCase, widgetAppearanceCase);
    const auto widgetAppearanceSection = section(
        widgetAppearanceCase, desktopCase);
    const auto desktopSection = section(desktopCase, pagesCase);
    const auto pagesSection = section(pagesCase, desktopIconsCase);
    const auto desktopIconsSection = section(
        desktopIconsCase, beautificationCase);
    const auto beautificationSection = section(
        beautificationCase, categoriesCase);
    const auto categoriesSection = section(categoriesCase, dockCase);
    const auto dockSection = section(dockCase, taskbarCase);
    const auto taskbarSection = section(taskbarCase, compatibilityCase);
    Check(personalizationSection.find(
              "personalizationPage_->ThemeContent()") !=
                std::string_view::npos &&
            personalizationSection.find("personalization.backgroundColor") !=
                std::string_view::npos &&
            personalizationSection.find("personalization.edgeHighlight") !=
                std::string_view::npos &&
            personalizationSection.find(
              "personalization.edgeHighlightWidth") !=
                std::string_view::npos &&
            personalizationSection.find(
              "personalization.edgeHighlightStrength") !=
                std::string_view::npos &&
            widgetAppearanceSection.find(
              "personalizationPage_->WidgetLayoutContent()") !=
                std::string_view::npos &&
            widgetAppearanceSection.find("personalization.backgroundColor") ==
                std::string_view::npos &&
            widgetAppearanceSection.find("desktop.categoryLayout") !=
                std::string_view::npos &&
            desktopSection.find(
              "generalPage_->DesktopBehaviorContent()") !=
                std::string_view::npos &&
            desktopSection.find("DesktopIconsContent()") ==
                std::string_view::npos &&
            pagesSection.find("pageLayoutPage_->Content()") !=
                std::string_view::npos &&
            pagesSection.find("generalPage_->PageNavigationContent()") !=
                std::string_view::npos &&
            pages.find("CanReorderItems(true)") != std::string::npos &&
            pages.find("dimensions.Orientation(muxc::Orientation::Vertical)") !=
                std::string::npos &&
            pages.find("NumberBoxSpinButtonPlacementMode::Inline") !=
                std::string::npos &&
            pages.find("columnsLabel.Text(L(\"settings.pages.columns\"))") !=
                std::string::npos &&
            pages.find("rowsLabel.Text(L(\"settings.pages.rows\"))") !=
                std::string::npos &&
            pages.find("UpdateOrderState();\n                ApplyOrder();") !=
                std::string::npos &&
            pages.find("[this](const auto&, const auto&) { ConfirmGrid(); }") !=
                std::string::npos &&
            pages.find("applyOrderButton") == std::string::npos &&
            pages.find("void ConfirmOrder()") == std::string::npos &&
            pages.find("applyGridButton") == std::string::npos &&
            pages.find("L(\"settings.dialog.confirm\")") !=
                std::string::npos &&
            pages.find("settings.pages.grid.confirm.title") !=
                std::string::npos &&
            pages.find("settings.pages.role.currentLast") !=
                std::string::npos &&
            pages.find("settings.pages.role.first") !=
                std::string::npos &&
            pages.find("PageListMatchesSnapshot") !=
                std::string::npos &&
            pages.find("RefreshPageRowsInPlace") !=
                std::string::npos &&
            pages.find("refreshTimer.Interval(std::chrono::milliseconds(500))") !=
                std::string::npos &&
            desktopIconsSection.find(
              "desktopPage_->DesktopIconsContent()") !=
                std::string_view::npos &&
            desktopIconsSection.find("desktop.iconSize") !=
                std::string_view::npos &&
            beautificationSection.find(
              "desktopPage_->IconBeautificationContent()") !=
                std::string_view::npos &&
            beautificationSection.find("desktop.iconBeautify.outline") !=
                std::string_view::npos &&
            desktopSection.find("desktop.tabFontSize") ==
                std::string_view::npos &&
            categoriesSection.find("desktopPage_->CategoryContent()") !=
                std::string_view::npos &&
            categoriesSection.find("desktop.categoryLayout") ==
                std::string_view::npos &&
            categoriesSection.find("desktop.categoryCounts") !=
                std::string_view::npos &&
            categoriesSection.find("desktop.categoryRules") !=
                std::string_view::npos &&
            dockSection.find("dockPage_->DockEnableContent()") !=
                std::string_view::npos &&
            dockSection.find("generalPage_->DockShortcutContent()") !=
                std::string_view::npos &&
            dockSection.find("dockPage_->DockContent()") !=
                std::string_view::npos &&
            dockSection.find("dockPage_->Activate();") !=
                std::string_view::npos &&
            dockSection.find("ActivateTaskbar") ==
                std::string_view::npos &&
            taskbarSection.find("dockPage_->TaskbarContent()") !=
                std::string_view::npos &&
            taskbarSection.find("dockPage_->ActivateTaskbar();") !=
                std::string_view::npos &&
            presenter.find("\"desktop.softwareDesktop\"") !=
                std::string::npos &&
            presenter.find("\"desktop.passthrough\"") !=
                std::string::npos &&
            presenter.find("\"desktop.doubleClickHide\"") !=
                std::string::npos &&
            shell.find("result.route.page = result.focusId.starts_with") ==
                std::string::npos,
        "Appearance leaves, Desktop, Pages, Categories, Dock, and Taskbar compose only their owned presenter sections and moved focus aliases remain stable");

    const auto generalItem = shellXaml.find("x:Name=\"GeneralItem\"");
    const auto homeItem = shellXaml.find("x:Name=\"HomeItem\"");
    const auto appearanceItem = shellXaml.find(
        "x:Name=\"PersonalizationItem\"");
    const auto appearanceThemeItem = shellXaml.find(
        "x:Name=\"AppearanceThemeItem\"");
    const auto appearanceWidgetsItem = shellXaml.find(
        "x:Name=\"AppearanceWidgetsItem\"");
    const auto appearanceDesktopIconsItem = shellXaml.find(
        "x:Name=\"AppearanceDesktopIconsItem\"");
    const auto appearanceIconBeautificationItem = shellXaml.find(
        "x:Name=\"AppearanceIconBeautificationItem\"");
    const auto desktopShellHeader = shellXaml.find(
        "x:Name=\"DesktopShellHeader\"");
    const auto desktopItem = shellXaml.find("x:Name=\"DesktopItem\"");
    const auto pagesItem = shellXaml.find("x:Name=\"PagesItem\"");
    const auto categoriesItem = shellXaml.find(
        "x:Name=\"CategoriesItem\"");
    const auto dockItem = shellXaml.find("x:Name=\"DockItem\"");
    const auto taskbarItem = shellXaml.find("x:Name=\"TaskbarItem\"");
    const auto widgetsItem = shellXaml.find("x:Name=\"WidgetsItem\"");
    const auto dataHeader = shellXaml.find("x:Name=\"DataHeader\"");
    const auto developerItem = shellXaml.find("x:Name=\"DeveloperItem\"");
    const auto backupItem = shellXaml.find("x:Name=\"BackupItem\"");
    const auto aboutItem = shellXaml.find("x:Name=\"AboutItem\"");
    const auto debugItem = shellXaml.find("x:Name=\"DebugItem\"");
    const auto homeCollapsed = shellXaml.find(
        "Visibility=\"Collapsed\"", homeItem);
    const auto shellRootStart = shellXaml.find(
        "<Grid x:Name=\"ShellRoot\"");
    const auto shellRootEnd = shellXaml.find('>', shellRootStart);
    const auto hiddenShellAcceleratorHint = shellXaml.find(
        "KeyboardAcceleratorPlacementMode=\"Hidden\"", shellRootStart);
    const auto shellSearchAccelerator = shellXaml.find(
        "x:Name=\"SearchKeyboardAccelerator\"");
    const auto paneSearchStart = shellXaml.find(
        "<NavigationView.PaneHeader>");
    const auto paneSearchEnd = shellXaml.find(
        "</NavigationView.PaneHeader>", paneSearchStart);
    const auto settingsSearchBox = shellXaml.find(
        "x:Name=\"SettingsSearchBox\"");
    const auto compactSearchButton = shellXaml.find(
        "x:Name=\"CompactSearchButton\"");
    Check(homeItem != std::string::npos &&
            homeItem < homeCollapsed && homeCollapsed < generalItem &&
            generalItem < appearanceItem &&
            appearanceItem < appearanceThemeItem &&
            appearanceThemeItem < appearanceWidgetsItem &&
            appearanceWidgetsItem < appearanceDesktopIconsItem &&
            appearanceDesktopIconsItem <
                appearanceIconBeautificationItem &&
            appearanceIconBeautificationItem < desktopShellHeader &&
            desktopShellHeader < desktopItem &&
            desktopItem < pagesItem && pagesItem < categoriesItem &&
            categoriesItem < dockItem &&
            dockItem < taskbarItem && taskbarItem < widgetsItem &&
            widgetsItem < dataHeader && dataHeader < backupItem &&
            backupItem < aboutItem && aboutItem < developerItem &&
            developerItem < debugItem &&
            shellXaml.find("NavigationView.FooterMenuItems") !=
                std::string::npos &&
            shellXaml.find("PaneDisplayMode=\"Auto\"") !=
                std::string::npos &&
            shellXaml.find("CompactModeThresholdWidth=\"720\"") !=
                std::string::npos &&
            shellXaml.find("ExpandedModeThresholdWidth=\"1008\"") !=
                std::string::npos &&
            shellXaml.find("IsPaneToggleButtonVisible=\"True\"") !=
                std::string::npos &&
            shellXaml.find("IsBackButtonVisible=\"Collapsed\"") !=
                std::string::npos &&
            shellXaml.find("x:Name=\"TitleBarBackButton\"") ==
                std::string::npos &&
            shellXaml.find("x:Name=\"TitleBarPaneToggleButton\"") ==
                std::string::npos &&
            shellSearchAccelerator != std::string::npos &&
            shellXaml.find("x:Name=\"SearchKeyboardAccelerator\"",
                shellSearchAccelerator + 1) == std::string::npos &&
            paneSearchStart < settingsSearchBox &&
            settingsSearchBox < paneSearchEnd &&
            shellXaml.find("x:Name=\"SettingsSearchBox\"",
                paneSearchEnd) == std::string::npos &&
            compactSearchButton != std::string::npos &&
            shellXaml.find("<NavigationView.AutoSuggestBox>") ==
                std::string::npos &&
            shellXaml.find("SelectsOnInvoked=\"False\"",
              appearanceItem) < appearanceThemeItem &&
            shell.find("AppearanceThemeItem().Content(") !=
                std::string::npos &&
            shell.find("appearance-theme.svg") != std::string::npos &&
            shell.find("appearance-widgets.svg") != std::string::npos &&
            shell.find("appearance-desktop-icons.svg") !=
                std::string::npos &&
            shell.find("appearance-icon-beautification.svg") !=
                std::string::npos &&
            shell.find("muxc::ImageIcon CreateColorNavigationIcon") !=
                std::string::npos &&
            shell.find("muxmi::SvgImageSource source") !=
                std::string::npos &&
            shell.find("CreateHighContrastNavigationIcon") !=
                std::string::npos &&
            shell.find("DesktopShellHeader().Content(") !=
                std::string::npos &&
            shell.find("DataHeader().Content(") != std::string::npos,
        "the adaptive NavigationView combines its built-in toggle with one PaneHeader search box, preserves a compact search affordance, uses full-color SVG icons with a High Contrast fallback, and exposes the navigation groups in task order");
    Check(shellHeader.find("titleBarBackToken_") == std::string::npos &&
            shellHeader.find("titleBarPaneToggleToken_") ==
                std::string::npos &&
            shell.find("TitleBarBackButton().Click") == std::string::npos &&
            shell.find("TitleBarPaneToggleButton().Click") ==
                std::string::npos &&
            shellXaml.find("x:Name=\"BackKeyboardAccelerator\"") !=
                std::string::npos &&
            shellXaml.find("x:Name=\"SearchKeyboardAccelerator\"") !=
                std::string::npos &&
            shellRootStart != std::string::npos &&
            hiddenShellAcceleratorHint < shellRootEnd &&
            shell.find("BackKeyboardAccelerator().Invoked") !=
                std::string::npos &&
            shell.find("SearchKeyboardAccelerator().Invoked") !=
                std::string::npos &&
            shell.find("navigation_.PeekBack()") != std::string::npos &&
            shell.find("routeRequested_(*route)") != std::string::npos &&
            shell.find("navigation_.GoBack()") != std::string::npos &&
            shell.find("const bool canGoBack = navigation_.CanGoBack()") !=
                std::string::npos &&
            shell.find("TitleBarBackButton().IsEnabled(canGoBack)") ==
                std::string::npos &&
            shell.find("BackKeyboardAccelerator().IsEnabled(canGoBack)") !=
                std::string::npos &&
            shell.find("BackKeyboardAccelerator().Invoked(") !=
                std::string::npos,
        "the native NavigationView owns pane expansion while controller-owned history remains available through Alt+Left and Ctrl+F still opens search without shell-wide accelerator tooltips");
    const auto numberDismissHandlerStart = shell.find(
        "ShellRoot().AddHandler(");
    const auto numberDismissHandlerEnd = shell.find(
        "true);", numberDismissHandlerStart);
    const auto handledPointerRegistration = shell.find(
        "shellPointerPressedHandler_", numberDismissHandlerStart);
    Check(shellHeader.find("shellPointerPressedHandler_") !=
                std::string::npos &&
            shell.find("winrt::box_value(muxi::PointerEventHandler{") !=
                std::string::npos &&
            shell.find("FindNumberBoxAncestor(focused)") !=
                std::string::npos &&
            shell.find("IsWithinNumberBox(args.OriginalSource()") !=
                std::string::npos &&
            shell.find("FocusManager::GetFocusedElement(") !=
                std::string::npos &&
            shell.find("control.Focus(mux::FocusState::Pointer)") !=
                std::string::npos &&
            shell.find("PageScrollViewer().Focus(mux::FocusState::Pointer)") !=
                std::string::npos &&
            numberDismissHandlerStart != std::string::npos &&
            shell.find("mux::UIElement::PointerPressedEvent()") !=
                std::string::npos &&
            handledPointerRegistration < numberDismissHandlerEnd &&
            shell.find("ShellRoot().RemoveHandler(") !=
                std::string::npos,
        "pointer presses outside a focused NumberBox move pointer focus within the settings window so compact spin buttons dismiss, including handled child events, and the routed handler is removed on close");

    const auto dismissStart = sharedControls.find("void Dismiss() noexcept");
    const auto dismissEnd = sharedControls.find("void Close() noexcept",
        dismissStart);
    const auto dismissCommit = sharedControls.find("Commit();",
        dismissStart);
    const auto dismissHide = sharedControls.find("flyout.Hide();",
        dismissStart);
    const auto cancelStart = sharedControls.find(
        "cancelToken = cancel.Click");
    const auto cancelEnd = sharedControls.find(
        "closedToken = flyout.Closed", cancelStart);
    const auto cancelRollback = sharedControls.find("Rollback();", cancelStart);
    const auto cancelHide = sharedControls.find("flyout.Hide();", cancelStart);
    const auto closedStart = sharedControls.find(
        "closedToken = flyout.Closed", cancelStart);
    const auto closedEnd = sharedControls.find("UpdateSwatch();", closedStart);
    const auto closedCommit = sharedControls.find("Commit();", closedStart);
    Check(sharedControls.find("struct SettingRow") != std::string::npos &&
            sharedControls.find("kSettingControlWidth = 300.0") !=
                std::string::npos &&
            sharedControls.find("kSettingRowStackThreshold = 700.0") !=
                std::string::npos &&
            sharedControls.find("muxc::Grid::SetColumn(text, 0)") !=
                std::string::npos &&
            sharedControls.find("muxc::Grid::SetColumn(controlHost, 1)") !=
                std::string::npos &&
            sharedControls.find(
                "root.RowDefinitions().Append(labelRow)") !=
                std::string::npos &&
            sharedControls.find(
                "root.RowDefinitions().Append(controlRow)") !=
                std::string::npos &&
            sharedControls.find("root.SizeChanged(") !=
                std::string::npos &&
            sharedControls.find("winrt::make_weak(controlColumn)") !=
                std::string::npos &&
            sharedControls.find("winrt::make_weak(text)") !=
                std::string::npos &&
            sharedControls.find("winrt::make_weak(controlHost)") !=
                std::string::npos &&
            sharedControls.find(
              "args.NewSize().Width < kSettingRowStackThreshold") !=
                std::string::npos &&
            sharedControls.find("grid.ColumnSpacing(stacked") !=
                std::string::npos &&
            sharedControls.find("grid.RowSpacing(stacked") !=
                std::string::npos &&
            sharedControls.find("Grid::SetRow(responsiveControlHost") !=
                std::string::npos &&
            sharedControls.find("void SetControlAlignment(") !=
                std::string::npos,
        "SettingRow keeps the 300-DIP wide editor column and safely stacks text above controls below its narrow-width threshold");
    Check(sharedControls.find("struct ColorFlyoutEditor") !=
                std::string::npos &&
            sharedControls.find("SettingsUpdateMode::Preview") !=
                std::string::npos &&
            sharedControls.find("SettingsUpdateMode::PreviewAndCommit") !=
                std::string::npos &&
            sharedControls.find("CoalescedPreviewTimer") !=
                std::string::npos &&
            sharedControls.find("kContinuousPreviewInterval{33}") !=
                std::string::npos &&
            sharedControls.find("Rollback()") != std::string::npos &&
            dismissStart != std::string::npos &&
            dismissCommit < dismissHide && dismissHide < dismissEnd &&
            cancelStart != std::string::npos &&
            cancelRollback < cancelHide && cancelHide < cancelEnd &&
            closedStart != std::string::npos &&
            closedCommit < closedEnd &&
            sharedControls.find("applyToken = apply.Click") ==
                std::string::npos &&
            sharedControls.find("actions.Children().Append(apply)") ==
                std::string::npos &&
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
        "continuous previews remain frame-coalesced and color flyouts light-dismiss by committing while explicit Cancel rolls back");

    const auto firstFontReset = desktop.find("}, 16.0);");
    const auto numericResetPublish = desktop.find("changed(defaultValue,");
    const auto numericResetCommit = desktop.find(
        "SettingsUpdateMode::PreviewAndCommit", numericResetPublish);
    const auto numericResetEnd = desktop.find(");", numericResetPublish);
    Check(presenter.find("quickNavigationHotkey.SetDefaultValue(") !=
                std::string::npos &&
            presenter.find(
              "Chord(MOD_CONTROL | MOD_ALT, VK_SPACE)") !=
                std::string::npos &&
            presenter.find(
              "previousPageHotkey.SetDefaultValue(Chord(0, VK_PRIOR))") !=
                std::string::npos &&
            presenter.find(
              "nextPageHotkey.SetDefaultValue(Chord(0, VK_NEXT))") !=
                std::string::npos &&
            presenter.find(
              "Chord(MOD_CONTROL | MOD_ALT, VK_OEM_3)") !=
                std::string::npos &&
            presenter.find("Chord(MOD_CONTROL | MOD_ALT, 'D')") !=
                std::string::npos &&
            presenter.find("actions.Children().Append(reset)") ==
                std::string::npos &&
            sharedControls.find(
              "inline void ConfigureRestoreDefaultButton(") !=
                std::string::npos &&
            sharedControls.find("button.Content(icon)") !=
                std::string::npos &&
            sharedControls.find("icon.FontSize(16.0)") !=
                std::string::npos &&
            sharedControls.find("ToolTipService::SetToolTip(") !=
                std::string::npos &&
            sharedControls.find(
              "AutomationProperties::SetHelpText(button, accessibleText)") !=
                std::string::npos &&
            presenter.find("row.Initialize(recorder.Root())") !=
                std::string::npos &&
            sharedControls.find(
              "controlHost.HorizontalAlignment(alignment)") !=
                std::string::npos &&
            sharedControls.find("toggle.MinWidth(0.0)") !=
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
            recorder.find("muxc::ContentDialog dialog") !=
                std::string::npos &&
            recorder.find(
              "dialog.DefaultButton(muxc::ContentDialogButton::Primary)") !=
                std::string::npos &&
            recorder.find(
              "ShowShortcutDialog(\n    std::shared_ptr<HotkeyRecorderState> state)") !=
                std::string::npos &&
            recorder.find(
              "ShowShortcutDialog(\n    const std::shared_ptr<HotkeyRecorderState>& state)") ==
                std::string::npos &&
            recorder.find("co_await state->dialog.ShowAsync()") !=
                std::string::npos &&
            recorder.find("captureSurface.KeyDown") !=
                std::string::npos &&
            recorder.find("ResolveRoutedVirtualKey(args)") !=
                std::string::npos &&
            recorder.find("args.KeyStatus()") != std::string::npos &&
            recorder.find("MapVirtualKeyExW(") != std::string::npos &&
            recorder.find("MAPVK_VSC_TO_VK_EX") != std::string::npos &&
            recorder.find("args.OriginalKey()") == std::string::npos &&
            recorder.find("state->rules.Commit()") !=
                std::string::npos &&
            recorder.find("state->rules.Restore(state->defaultValue)") !=
                std::string::npos &&
            recorder.find("state->rules.Clear()") !=
                std::string::npos &&
            recorder.find("LostFocus(") == std::string::npos &&
            recorderRules.find(
              "virtualKey == KeyEnter || virtualKey == KeyEscape") !=
                std::string::npos &&
            recorderRules.find("HotkeyRecorderTransition Commit()") !=
                std::string::npos &&
            recorderRules.find("HotkeyRecorderTransition Restore(") !=
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
            dock.find("nullptr, 100.0") != std::string::npos,
        "hotkeys use a PowerToys-style explicit edit dialog with compact previews, default/clear actions, and no focus-loss auto-commit while numeric defaults retain accessible reset actions");
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
