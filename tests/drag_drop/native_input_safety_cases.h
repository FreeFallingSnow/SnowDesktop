#pragma once

namespace native_input_safety_cases
{
inline void Run()
{
    const auto name = L"SnowDesktopInputSafety-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) throw std::runtime_error("create inactive input-safety desktop");
    std::exception_ptr failure;
    std::thread worker([&] {
        try
        {
            if (!SetThreadDesktop(desktop)) throw std::runtime_error("select private input-safety thread desktop");
            native_drag_commit_cases::Profile profile;
            NativeDragIntegrationAccess::RunInputSafety();
        }
        catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    CloseDesktop(desktop);
    if (failure) std::rethrow_exception(failure);
}
}
