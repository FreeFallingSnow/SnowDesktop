#pragma once
#include "system/status_bar/status_bar_interaction.h"

namespace native_status_selection_cases
{
inline void Run()
{
    const auto name = L"SnowDesktopStatusSelection-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) throw std::runtime_error("create inactive R13 test desktop");
    std::exception_ptr failure;
    std::thread worker([&] {
        try
        {
            if (!SetThreadDesktop(desktop)) throw std::runtime_error("select inactive R13 thread desktop");
            native_drag_commit_cases::Profile profile;
            native_list_hover_cases::Rendering rendering;
            auto& app = profile.Model(false, profile.root / L"status-selection");
            NativeDragIntegrationAccess::RunStatusSelection(app, rendering.factory.Get(), rendering.device.Get(),
                rendering.context.Get(), rendering.target.Get());
        }
        catch (...) { failure = std::current_exception(); }
    });
    worker.join(); CloseDesktop(desktop);
    if (failure) std::rethrow_exception(failure);
}
}
