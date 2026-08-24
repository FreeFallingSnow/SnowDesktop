#include "winui/settings_shell_navigation.h"

#include <cstdlib>
#include <iostream>

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
}

int main()
{
    TestHistoryAndFocusRoutes();
    TestConditionalPages();
    TestControllerGenerationGate();
    TestInvalidRoutes();

    if (failures != 0)
    {
        std::cerr << failures << " WinUI settings navigation check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI settings navigation checks passed\n";
    return EXIT_SUCCESS;
}
