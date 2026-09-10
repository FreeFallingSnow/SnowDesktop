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

void TestLargeIconParentNavigation()
{
    SettingsRoute first = SettingsRoute::ForPage(SettingsPage::LargeIcon); first.itemKey = L"first";
    SettingsRoute second = first; second.itemKey = L"second";
    SettingsShellNavigationState direct;
    Check(direct.ApplyControllerUpdate(first, 1, 1) && direct.CanGoBack() &&
        direct.PeekBack()->page == SettingsPage::AppearanceDesktopIcons,
        "a directly opened large-icon editor always has the Icons parent");
    Check(direct.ApplyControllerUpdate(second, 2, 1) && direct.HistorySize() == 2 &&
        direct.PeekBack()->page == SettingsPage::AppearanceDesktopIcons,
        "switching the edited item replaces the detail entry, not its parent");
    const auto parent = SettingsRoute::ForPage(SettingsPage::AppearanceDesktopIcons, "desktop.iconSize");
    SettingsShellNavigationState existing;
    Check(existing.Navigate(parent) && existing.Navigate(first) && existing.PeekBack() == parent,
        "entering a detail preserves its existing parent focus route");
    Check(existing.Navigate(second) && existing.GoBack() == parent,
        "Back skips the previous item editor and restores the parent focus target");
    Check(direct.ApplyControllerUpdate(second, 1, 2) && direct.HistorySize() == 2,
        "reopening a settings generation seeds a fresh Icons parent");
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
    TestControllerCommittedBackNavigation();
    TestInvalidRoutes();
    TestLargeIconParentNavigation();
    if (failures != 0)
    {
        std::cerr << failures << " WinUI settings navigation check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI settings navigation checks passed\n";
    return EXIT_SUCCESS;
}
