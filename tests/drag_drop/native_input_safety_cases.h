#pragma once
#include <imm.h>
#include <iterator>
#include <string_view>

namespace native_input_safety_cases
{
inline constexpr std::wstring_view kInitialDesktopPrefix = L"SnowDesktopInputSafety-";

inline void RunWorkerOnInitialDesktop(std::wstring_view expectedDesktopName)
{
    wchar_t actual[256]{};
    DWORD needed = 0;
    if (!expectedDesktopName.starts_with(kInitialDesktopPrefix) || expectedDesktopName.size() >= std::size(actual) ||
        !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, actual, sizeof(actual), &needed) ||
        std::wstring_view(actual) != expectedDesktopName)
        throw std::runtime_error("input-safety worker must start on its exact owned private desktop");
    // Preserve the existing thread-level IME policy. The desktop now belongs
    // to the whole process from its creation, including graphical services.
    if (!ImmDisableIME(GetCurrentThreadId()))
        throw std::runtime_error("disable IME on the initial private input-safety thread");
    std::cerr << "Input safety child: initial private desktop verified\n";
    {
        native_drag_commit_cases::Profile profile;
        NativeDragIntegrationAccess::RunInputSafety();
        std::cerr << "Input safety child: all native assertions complete\n";
    }
    std::cerr << "Input safety child: profile and COM cleanup complete\n";
}

inline void RunOnInitialDesktopProcess()
{
    // Like the existing R09 fixture, this model has process-lifetime owners.
    // Start graphical/COM services on their actual initial desktop instead of
    // moving one GUI thread after the test process has already initialized.
    struct Owned
    {
        HDESK desktop = nullptr;
        HANDLE job = nullptr, process = nullptr, thread = nullptr;
        bool assigned = false;
        ~Owned()
        {
            if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            {
                if (assigned && job) TerminateJobObject(job, 1);
                else TerminateProcess(process, 1); // Only our still-suspended child before job assignment.
            }
            if (job) CloseHandle(job); // Kill-on-close also contains owned descendants on an error.
            if (process) { WaitForSingleObject(process, 5000); CloseHandle(process); }
            if (thread) CloseHandle(thread);
            if (desktop) CloseDesktop(desktop);
        }
    } owned;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto name = std::wstring(kInitialDesktopPrefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    owned.desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    require(owned.desktop != nullptr, "create never-activated initial input-safety desktop");
    owned.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(owned.job && SetInformationJobObject(owned.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
        "bound the owned input-safety child lifetime");
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    require(length != 0 && length < std::size(executable) && std::filesystem::path(executable).is_absolute(),
        "resolve the absolute current compiled input-safety test executable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --input-safety-worker \"" + name + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = const_cast<wchar_t*>(name.c_str());
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &child) != FALSE,
        "create the owned input-safety process on its initial private desktop");
    owned.process = child.hProcess; owned.thread = child.hThread;
    require(AssignProcessToJobObject(owned.job, owned.process) != FALSE,
        "contain the suspended input-safety child before it executes");
    owned.assigned = true;
    require(ResumeThread(owned.thread) != static_cast<DWORD>(-1), "resume the contained input-safety child");
    // Leave time inside the unchanged 60-second CTest budget for failure
    // cleanup and the parent's existing status-selection checks.
    require(WaitForSingleObject(owned.process, 45000) == WAIT_OBJECT_0,
        "initial-desktop input-safety child must finish naturally within its existing test budget");
    DWORD exitCode = 1;
    require(GetExitCodeProcess(owned.process, &exitCode) && exitCode == 0,
        "input-safety child must naturally pass its full assertions and COM/window cleanup");
    const bool closed = CloseDesktop(owned.desktop) != FALSE;
    if (!closed) std::cerr << "Input safety parent: desktop close error=" << GetLastError() << '\n';
    if (closed) owned.desktop = nullptr;
    require(closed, "close the owned input-safety desktop after its child exits");
    std::cerr << "Input safety parent: child exited and owned desktop closed\n";
}

inline void Run(bool privateRenameOnly = false)
{
    if (privateRenameOnly) std::cerr << "Private rename wrapper: input snapshot\n";
    POINT cursorBefore{};
    if (!GetCursorPos(&cursorBefore)) throw std::runtime_error("snapshot input desktop cursor before native safety fixtures");
    const HWND foregroundBefore = GetForegroundWindow(), focusBefore = GetFocus();
    const auto inputUnchanged = [&] {
        POINT cursorAfter{};
        if (!GetCursorPos(&cursorAfter) || cursorBefore.x != cursorAfter.x || cursorBefore.y != cursorAfter.y ||
            foregroundBefore != GetForegroundWindow() || focusBefore != GetFocus())
            throw std::runtime_error("inactive native safety fixtures changed input desktop cursor, foreground or focus");
    };
    if (!privateRenameOnly)
    {
        std::exception_ptr failure;
        try { RunOnInitialDesktopProcess(); }
        catch (...) { failure = std::current_exception(); }
        inputUnchanged();
        if (failure) std::rethrow_exception(failure);
        return;
    }
    const auto name = std::wstring(privateRenameOnly ? L"SnowDesktopPrivateRename-" : L"SnowDesktopInputSafety-") +
        std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) throw std::runtime_error("create inactive input-safety desktop");
    std::exception_ptr failure;
    std::thread worker([&] {
        try
        {
            if (privateRenameOnly) std::cerr << "Private rename wrapper: worker desktop\n";
            if (!SetThreadDesktop(desktop)) throw std::runtime_error("select private input-safety thread desktop");
            // Native rename receives committed WM_CHAR data without starting
            // IME composition. Keep the inactive desktop independent of the
            // input-desktop IME monitor during fixture teardown.
            if (!ImmDisableIME(GetCurrentThreadId()))
                throw std::runtime_error("disable IME on the isolated input-safety thread");
            if (privateRenameOnly) std::cerr << "Private rename wrapper: temporary profile\n";
            native_drag_commit_cases::Profile profile;
            if (privateRenameOnly) std::cerr << "Private rename wrapper: scene enter\n";
            if (privateRenameOnly) NativeDragIntegrationAccess::RunPrivateRenameInput();
            else NativeDragIntegrationAccess::RunInputSafety();
            if (privateRenameOnly) std::cerr << "Private rename wrapper: scene returned\n";
        }
        catch (...) { failure = std::current_exception(); }
    });
    if (privateRenameOnly) std::cerr << "Private rename wrapper: joining worker\n";
    worker.join();
    if (privateRenameOnly) std::cerr << "Private rename wrapper: worker joined\n";
    CloseDesktop(desktop);
    inputUnchanged();
    if (failure) std::rethrow_exception(failure);
    if (privateRenameOnly)
        std::cout << "Private rename outer finish: input-desktop cursor, foreground and focus unchanged\n" << std::flush;
}
}
