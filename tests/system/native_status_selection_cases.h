#pragma once
#include "system/status_bar/status_bar_interaction.h"
#include <imm.h>

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
            // These test windows never accept text. Avoid initializing the
            // system IME monitor on an inactive desktop: its teardown can wait
            // for an interactive desktop service while unwinding a failure.
            if (!ImmDisableIME(GetCurrentThreadId()))
                throw std::runtime_error("disable IME on the isolated R13 worker thread");
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
