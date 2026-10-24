#pragma once

namespace native_drag_foreground_damage_cases
{
inline void RunOriginalFullRequestControl(const std::filesystem::path& root)
{
    const auto require = [](bool ok, const char* message) {
        native_drag_commit_cases::Require(ok, message);
    };
    struct OwnedControl
    {
        HANDLE output = INVALID_HANDLE_VALUE;
        HANDLE input = INVALID_HANDLE_VALUE;
        HANDLE process = nullptr;
        ~OwnedControl()
        {
            if (process)
            {
                if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
                {
                    // Only the exact child created below is owned here.
                    TerminateProcess(process, 88);
                    WaitForSingleObject(process, 5000);
                }
                CloseHandle(process);
            }
            if (output && output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (input && input != INVALID_HANDLE_VALUE) CloseHandle(input);
        }
    } control;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const auto logPath = root / L"negative-full-client.log";
    control.output = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    control.input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(control.output != INVALID_HANDLE_VALUE && control.input != INVALID_HANDLE_VALUE,
        "create private negative-control output and input handles");
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    require(count > 0 && count < std::size(executable), "resolve the actual current test executable");
    std::wstring command = L"\"" + std::wstring(executable, count) + L"\" --r05-full-reference";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = control.output;
    startup.hStdInput = control.input;
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE,
        "run original full-client control with the same built test executable");
    control.process = child.hProcess;
    CloseHandle(child.hThread);
    CloseHandle(control.output); control.output = nullptr;
    CloseHandle(control.input); control.input = nullptr;
    require(WaitForSingleObject(control.process, 45000) == WAIT_OBJECT_0,
        "original full-client control must reach its business assertion within its deadline");
    DWORD exitCode = 0;
    require(GetExitCodeProcess(control.process, &exitCode) != FALSE,
        "read actual original-control process exit code");
    const auto output = native_drag_commit_cases::Read(logPath);
    std::cout << "R05 expected negative control: current executable --r05-full-reference, exit="
        << exitCode << '\n' << output;
    require(exitCode == 1 &&
        output.find("R05 negative control executed original full-client foreground presentation") != std::string::npos &&
        output.find("R05 changed pointer feedback still requests the entire client") != std::string::npos,
        "old full-client path must fail the exact narrowed-area assertion, not startup or an unknown mode");
}

inline void Run(bool originalFullRequest = false)
{
    native_drag_commit_cases::Profile profile;
    auto& app = profile.Model(false, profile.root / L"pointer-foreground-damage");
    NativeDragIntegrationAccess::CheckDragForegroundDamage(app, originalFullRequest);
    native_drag_commit_cases::Require(NativeDragIntegrationAccess::NoPresentationWindows(app),
        "pointer damage regression never initializes the desktop host");
    std::cout << "Actual pointer foreground old/new pixels, target state and synchronous commit match the complete reference\n";
    if (!originalFullRequest) RunOriginalFullRequestControl(profile.root);
}
}
