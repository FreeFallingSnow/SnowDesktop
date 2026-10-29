#pragma once
#include <imm.h>

namespace native_input_safety_cases
{
inline void Run(bool privateRenameOnly = false)
{
    POINT cursorBefore{};
    if (!GetCursorPos(&cursorBefore)) throw std::runtime_error("snapshot input desktop cursor before native safety fixtures");
    const HWND foregroundBefore = GetForegroundWindow(), focusBefore = GetFocus();
    const auto name = std::wstring(privateRenameOnly ? L"SnowDesktopPrivateRename-" : L"SnowDesktopInputSafety-") +
        std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) throw std::runtime_error("create inactive input-safety desktop");
    std::exception_ptr failure;
    std::thread worker([&] {
        try
        {
            if (!SetThreadDesktop(desktop)) throw std::runtime_error("select private input-safety thread desktop");
            // Native rename receives committed WM_CHAR data without starting
            // IME composition. Keep the inactive desktop independent of the
            // input-desktop IME monitor during fixture teardown.
            if (!ImmDisableIME(GetCurrentThreadId()))
                throw std::runtime_error("disable IME on the isolated input-safety thread");
            native_drag_commit_cases::Profile profile;
            if (privateRenameOnly) NativeDragIntegrationAccess::RunPrivateRenameInput();
            else NativeDragIntegrationAccess::RunInputSafety();
        }
        catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    CloseDesktop(desktop);
    POINT cursorAfter{};
    if (!GetCursorPos(&cursorAfter) || cursorBefore.x != cursorAfter.x || cursorBefore.y != cursorAfter.y ||
        foregroundBefore != GetForegroundWindow() || focusBefore != GetFocus())
        throw std::runtime_error("inactive native safety fixtures changed input desktop cursor, foreground or focus");
    if (failure) std::rethrow_exception(failure);
    if (privateRenameOnly)
        std::cout << "Private rename outer finish: input-desktop cursor, foreground and focus unchanged\n";
}
}
