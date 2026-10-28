#pragma once

namespace native_dock_hover_cases
{
inline void CheckOriginalRoute(native_drag_commit_cases::Profile& profile)
{
    using native_drag_commit_cases::Require;
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    Require(length > 0 && length < std::size(executable), "R08 resolves the original-route control executable");
    const auto log = profile.root / L"r08-original-route.log";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    struct CloseOutput
    {
        HANDLE value;
        ~CloseOutput() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Require(output.value != INVALID_HANDLE_VALUE, "R08 creates an isolated original-route log");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = output.value; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --dock-hover-legacy";
    const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    CloseHandle(output.value); output.value = INVALID_HANDLE_VALUE;
    Require(created != FALSE, "R08 starts the test-owned original-route child under the same CTest binary lease");
    const DWORD waited = WaitForSingleObject(child.hProcess, 30000);
    if (waited != WAIT_OBJECT_0)
    {
        TerminateProcess(child.hProcess, 99); // Only this fixture's child.
        WaitForSingleObject(child.hProcess, 5000);
    }
    DWORD code = 0;
    const BOOL readExit = GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    Require(waited == WAIT_OBJECT_0 && readExit && code == 1,
        "R08 original route must finish with its expected business failure");
    const auto evidence = native_drag_commit_cases::Read(log);
    Require(evidence.find("R08 negative: executing frozen all-Dock hover damage route") != std::string::npos &&
        evidence.find("R08: unrelated screen must not be included in the hover update") != std::string::npos,
        "R08 control must execute the original fault and fail at the unrelated-screen assertion");
    std::cout << "R08 original-route control: --dock-hover-legacy; expected exit=1\n" << evidence;
}

inline void Run(bool legacy = false)
{
    if (!legacy)
    {
        std::exception_ptr failure;
        const std::wstring name = L"SnowDesktop.DockFirstFrame." + std::to_wstring(GetCurrentProcessId());
        const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        native_drag_commit_cases::Require(desktop != nullptr, "create a private first-frame test desktop");
        std::thread([&] {
            try
            {
                native_drag_commit_cases::Require(SetThreadDesktop(desktop) != FALSE,
                    "first-frame fixture uses a desktop that is never switched to");
                ImmDisableIME(0);
                native_drag_commit_cases::Profile frameProfile;
                auto& frameApp = NativeDragIntegrationAccess::CreateDockFrameScene(frameProfile.root / L"dock-first-frame");
                frameProfile.apps.push_back(&frameApp);
                NativeDragIntegrationAccess::CheckDockFrameTransitions(frameApp);
            }
            catch (...) { failure = std::current_exception(); }
        }).join();
        const bool closed = CloseDesktop(desktop) != FALSE;
        if (failure) std::rethrow_exception(failure);
        native_drag_commit_cases::Require(closed, "release the private first-frame test desktop");
    }
    native_drag_commit_cases::Profile profile;
    auto& app = profile.Model(false, profile.root / L"dock-hover");
    NativeDragIntegrationAccess::CheckDockHoverFrameRouting(app, legacy);
    native_drag_commit_cases::Require(NativeDragIntegrationAccess::NoPresentationWindows(app),
        "R08 routing regression never initializes a desktop presentation host");
    if (!legacy) CheckOriginalRoute(profile);
}
}
