#include "test_source_boundary.h"
#include "../src/winui/home_about_page_model.h"
#include "../src/settings/settings_window_open_rules.h"

namespace
{
bool CheckWindowPlacement()
{
    namespace placement = snowdesktop::settings_window_placement;
    bool passed = true;
    const auto check = [&passed](bool condition, const char* message) {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        passed = condition && passed;
    };
    const auto equal = [](const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top &&
            a.right == b.right && a.bottom == b.bottom;
    };
    const auto inside = [](const RECT& bounds, const RECT& work) {
        return bounds.left >= work.left && bounds.top >= work.top &&
            bounds.right <= work.right && bounds.bottom <= work.bottom;
    };

    const RECT topBarWork{0, 48, 1920, 1040};
    const auto centered = placement::Center({1120, 800}, topBarWork);
    check(equal(centered, {400, 144, 1520, 944}),
        "normal settings bounds center inside the AppBar work area, not the full screen");

    // A 150%/200% default client plus the native frame can exceed a laptop's
    // work height. The old y=max(0, ...) put the caption behind the top bar.
    const auto scaled = placement::Center({1680, 1200}, topBarWork);
    check(equal(scaled, {120, 48, 1800, 1040}),
        "oversized high-DPI settings fit below the top AppBar with all edges reachable");
    const auto twiceScaled = placement::Center({2240, 1600}, topBarWork);
    check(equal(twiceScaled, topBarWork),
        "a window larger than both work dimensions cannot retain an off-screen edge");

    const RECT leftMonitor{-1920, -200, 0, 800};
    check(equal(placement::Center({1120, 800}, leftMonitor),
            {-1520, -100, -400, 700}),
        "secondary monitors retain their negative screen coordinates");
    const RECT bottomTaskbar{1920, 0, 3840, 1024};
    check(equal(placement::Center({1120, 800}, bottomTaskbar),
            {2320, 112, 3440, 912}),
        "secondary-monitor placement uses its own work origin and bottom reservation");

    const RECT accessible{440, 160, 1440, 860};
    check(equal(placement::Constrain(accessible, topBarWork), accessible),
        "reopening an accessible settings window preserves the user's position and size");
    check(equal(placement::Constrain({440, 0, 1440, 700}, topBarWork),
            {440, 48, 1440, 748}),
        "a changed top work boundary moves the caption below the bar without resizing");
    const auto afterDisplayRemoval = placement::Constrain(
        {-1500, 150, -380, 950}, topBarWork);
    check(equal(afterDisplayRemoval, {0, 150, 1120, 950}),
        "normal bounds from a removed monitor become reachable on the remaining work area");
    const auto oversizedSavedWindow = placement::Constrain(
        {-2100, -260, 140, 1340}, leftMonitor);
    check(equal(oversizedSavedWindow, leftMonitor) && inside(oversizedSavedWindow, leftMonitor),
        "reopening oversized saved bounds fits the available work area");

    const auto normalMinimum = placement::FitSize({856, 559}, topBarWork);
    check(normalMinimum.cx == 856 && normalMinimum.cy == 559,
        "the ordinary minimum remains unchanged when the display can fit it");
    const auto limitedMinimum = placement::FitSize({1712, 1118}, {0, 64, 1280, 720});
    check(limitedMinimum.cx == 1280 && limitedMinimum.cy == 656,
        "minimum track dimensions never force the caption or bottom edge outside a small work area");
    check(equal(placement::Constrain(accessible, {}), accessible),
        "invalid work samples do not move an existing window");
    return passed;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    bool passed = snowdesktop::test::CheckSourceBoundaries(argv[1], {
        {"src/winui/settings_window_host.cpp", "", "",
         {"DesktopWindowXamlSource", "ImGui", "ID3D11", "IDXGISwapChain", "Present(",
          "SetDragRectangles(", "InputNonClientPointerSource", "SetRegionRects(",
          "WM_NCHITTEST", "WM_NCCALCSIZE", "DwmExtendFrameIntoClientArea",
          "HeapOptimizeResources", "EmptyWorkingSet("}},
        {"src/winui/SettingsShell.xaml", "", "",
         {"x:Name=\"MinimizeButton\"", "x:Name=\"MaximizeButton\"", "x:Name=\"CloseButton\"",
          "<NavigationView.Template>"}},
        // Native dragging owns the WM_DPICHANGED suggestion. Startup/off-screen
        // recovery must never constrain it against the previous monitor.
        {"src/winui/settings_window_host.cpp", "case WM_DPICHANGED:", "case WM_SETTINGCHANGE:",
         {"settings_window_placement::Constrain", "SettingsWindowWorkArea("}},
    });
    passed = CheckWindowPlacement() && passed;
    using namespace snowdesktop::winui;
    // Reopening settings while an experiment is active used to show Debug
    // only until the off switch was clicked, then remove that very page.
    DebugPageSession normal, resumed;
    passed = !normal.Visible(false) && resumed.Visible(true) &&
        resumed.Visible(false) && resumed.Visible(true) && resumed.Visible(false) && passed;
    normal.Unlock();
    passed = normal.Visible(false) && passed;
    DebugPageSession nextSession;
    passed = !nextSession.Visible(false) && passed;
    // Runtime-only pushes must remain orderable without controller edits,
    // including the requested read following an off publication.
    HomeAboutStatusSequence status;
    const auto on = status.Next(8);
    const auto off = status.Next(8);
    const auto read = status.Next(8);
    const auto reopened = status.Next(9);
    passed = on.generation == 8 && off.generation == 8 && read.generation == 8 &&
        on.revision < off.revision && off.revision < read.revision &&
        reopened.generation == 9 && read.revision < reopened.revision && passed;
    if (!passed) std::cerr << "FAIL: settings session visibility/publication contract\n";
    if (passed)
        std::cout << "Settings host source, model and placement contracts passed; no live UI behavior was exercised.\n";
    return passed ? 0 : 1;
}
