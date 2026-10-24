#pragma once

namespace native_foreground_culling_cases
{
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    auto& app = profile.Model(false, profile.root / L"foreground-culling");
    NativeDragIntegrationAccess::CheckForegroundCulling(app);
    native_drag_commit_cases::Require(NativeDragIntegrationAccess::NoPresentationWindows(app),
        "foreground culling never initializes desktop presentation windows");
    std::cout << "Actual foreground local pixels, Dock draw counts and tooltip repair match the full reference\n";
}
}
