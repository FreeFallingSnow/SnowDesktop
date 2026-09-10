#include "test_source_boundary.h"

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const bool passed = snowdesktop::test::CheckSourceBoundaries(argv[1], {
        {"src/winui/settings_window_host.cpp", "", "",
         {"DesktopWindowXamlSource", "ImGui", "ID3D11", "IDXGISwapChain", "Present(",
          "SetDragRectangles(", "InputNonClientPointerSource", "SetRegionRects(",
          "WM_NCHITTEST", "WM_NCCALCSIZE", "DwmExtendFrameIntoClientArea",
          "HeapOptimizeResources", "EmptyWorkingSet("}},
        {"src/winui/SettingsShell.xaml", "", "",
         {"x:Name=\"MinimizeButton\"", "x:Name=\"MaximizeButton\"", "x:Name=\"CloseButton\"",
          "<NavigationView.Template>"}},
    });
    if (passed)
        std::cout << "Settings host source boundaries passed; no UI behavior was exercised.\n";
    return passed ? 0 : 1;
}
