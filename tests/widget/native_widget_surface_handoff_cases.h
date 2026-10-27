#pragma once
#include "../drag_drop/native_widget_drag_hover_cases.h"

namespace native_widget_surface_handoff_cases
{
inline void Run()
{
    {
        native_drag_commit_cases::Profile profile;
        auto& app = profile.Model(false, profile.root / L"surface-handoff");
        NativeDragIntegrationAccess::CheckWidgetSurfaceHandoff(app);
        native_drag_commit_cases::Require(NativeDragIntegrationAccess::NoPresentationWindows(app),
            "surface handoff regression never opens a desktop presentation window");
        std::cout << "Actual Lua composition handoff keeps retained pixels hidden until the current surface is ready\n";
    }
    native_widget_drag_hover_cases::Run();
}
}
