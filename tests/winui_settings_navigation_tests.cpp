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

void TestNavigationFeedbackLifetime()
{
    // Production shell publications can repeat the same route after edits or
    // relayout. Those refreshes must not replay the locator or restore a banner
    // the user dismissed. This state is shared by the real shell entry points.
    SettingsShellNavigationFeedback feedback;
    auto theme = SettingsRoute::ForPage(SettingsPage::AppearanceTheme, "personalization.theme");
    theme.guideTopic = "theme";
    Check(feedback.UpdateRoute(theme, 4) && feedback.ShowGuideReturn() &&
        feedback.ConsumeHighlight(), "a guide destination offers one card locator and a return banner");
    Check(!feedback.ConsumeHighlight(), "repeated layout cannot restart the running or completed locator");
    Check(!feedback.UpdateRoute(theme, 4) && !feedback.ConsumeHighlight(),
        "an ordinary settings refresh cannot replay a completed locator");

    feedback.DismissGuideReturn();
    Check(!feedback.ShowGuideReturn() && !feedback.UpdateRoute(theme, 4) &&
        !feedback.ShowGuideReturn() && !feedback.ConsumeHighlight(),
        "dismissal survives a same-route refresh and cancels any pending locator");

    auto startup = SettingsRoute::ForPage(SettingsPage::General, "general.autoStart");
    startup.guideTopic = "startup";
    Check(feedback.UpdateRoute(startup, 4) && !feedback.ShowGuideReturn() &&
        feedback.ConsumeHighlight(), "General locates its setting without a redundant return banner");

    Check(feedback.UpdateRoute(theme, 4) && feedback.ShowGuideReturn() && feedback.ConsumeHighlight(),
        "returning through the guide starts fresh feedback after leaving the destination");
    feedback.DismissGuideReturn();
    feedback.Restart();
    Check(feedback.ShowGuideReturn() && feedback.ConsumeHighlight() && !feedback.ConsumeHighlight(),
        "an explicit repeated navigation rearms one locator and the return banner");

    Check(feedback.UpdateRoute(theme, 5) && feedback.ShowGuideReturn() && feedback.ConsumeHighlight(),
        "a new window session discards only ephemeral feedback state");
    auto dock = SettingsRoute::ForPage(SettingsPage::Dock, "dock.enable");
    feedback.UpdateRoute(dock, 5);
    Check(!feedback.ShowGuideReturn() && feedback.ConsumeHighlight(),
        "a normal search route gets a locator without guide chrome");
    feedback.UpdateRoute(SettingsRoute::ForPage(SettingsPage::Dock), 5);
    Check(!feedback.ConsumeHighlight(), "opening a page without a setting never starts a locator");

    feedback.UpdateRoute(theme, 5);
    feedback.CancelHighlight();
    Check(!feedback.UpdateRoute(theme, 5) && !feedback.ConsumeHighlight() && feedback.ShowGuideReturn(),
        "hiding settings cancels pending highlights without dismissing the return banner");
    feedback.UpdateRoute(dock, 5);
    feedback.DismissGuideReturn();
    Check(!feedback.ConsumeHighlight(), "dismissal also prevents a queued first locator from starting");
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
    TestNavigationFeedbackLifetime();
    {
        SettingsShellNavigationState bars;
        Check(bars.Navigate(SettingsRoute::ForPage(SettingsPage::DockAndTaskbar, "taskbar.theme")) &&
                bars.Route().page == SettingsPage::Taskbar,
            "moving the taskbar in navigation must preserve legacy taskbar routes");
        Check(bars.Navigate(SettingsRoute::ForPage(SettingsPage::StatusBar, "statusBar.enable")) &&
                bars.Route().page == SettingsPage::StatusBar && SettingsPageKey(bars.Route().page) == "status-bar",
            "the appended status bar destination must support focus and stable route keys");
        const auto back = bars.GoBack();
        Check(back && back->page == SettingsPage::Taskbar,
            "desktop bar pages must preserve navigation history");
    }
    if (failures != 0)
    {
        std::cerr << failures << " WinUI settings navigation check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI settings navigation checks passed\n";
    return EXIT_SUCCESS;
}
