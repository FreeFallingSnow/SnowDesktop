#include "test_source_boundary.h"
#include "../src/winui/home_about_page_model.h"

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
    });
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
        std::cout << "Settings host source boundaries passed; no UI behavior was exercised.\n";
    return passed ? 0 : 1;
}
