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
    Check(state.Route().page == SettingsPage::Home && !state.CanGoBack(),
        "navigation starts at Home without back history");

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

    Check(!state.SetVisibility({true, true}).has_value() &&
            state.Navigate(
                SettingsRoute::ForPage(SettingsPage::DeveloperTools)) &&
            state.Navigate(SettingsRoute::ForPage(SettingsPage::Debug)),
        "conditional routes become available only after their gates open");

    const auto replacement = state.SetVisibility({false, false});
    Check(replacement && replacement->page == SettingsPage::Home &&
            !state.CanGoBack(),
        "closing a conditional gate removes hidden history and returns Home");
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
    const std::string recorder = ReadText(
        root / "src/winui/hotkey_recorder.cpp");
    const std::string recorderHeader = ReadText(
        root / "src/winui/hotkey_recorder.h");

    Check(!shell.empty() && !shellHeader.empty() && !presenter.empty() &&
            !presenterHeader.empty() && !recorder.empty() &&
            !recorderHeader.empty(),
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
    Check(shell.find("generalPage_->Deactivate()") != std::string::npos &&
            presenter.find("void GeneralPagePresenter::Deactivate()") !=
                std::string::npos,
        "leaving or closing General cancels any active recorder");
}
}

int main(int argc, char** argv)
{
    TestHistoryAndFocusRoutes();
    TestConditionalPages();
    TestControllerGenerationGate();
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
