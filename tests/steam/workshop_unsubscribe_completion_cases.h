#pragma once

namespace workshop_unsubscribe_completion_cases
{
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    auto& app = profile.Model(false, profile.root / L"unsubscribe-layout");
    NativeDragIntegrationAccess::CheckWorkshopUnsubscribeCompletion(app);
    if (IsWindowVisible(NativeDragIntegrationAccess::completionWindow))
        throw std::runtime_error("Workshop completion regression must not show a desktop window");
    std::cout << "Actual host scopes unsubscribe completion to its item while preserving full-sync failures\n";
}
}
