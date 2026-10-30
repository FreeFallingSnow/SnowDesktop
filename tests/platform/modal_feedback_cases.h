#pragma once

#include "platform/operation_feedback.h"
#include "dock/dock_magnification.h"

namespace modal_feedback_cases
{
inline void CheckDialog(bool bridge)
{
    using namespace snowdesktop;
    using native_drag_commit_cases::Require;
    UiAnimationScheduler scheduler;
    Require(scheduler.Initialize(), "modal feedback initializes the production scheduler");
    dock_magnification::HoverEntryAnimation hover;
    hover.SetHovered(true, UiAnimationScheduler::MonotonicMilliseconds(), 1.0);
    int frames = 0, flushes = 0, timerCallbacks = 0;
    float observedScale = 1.0f;
    scheduler.StartAnimation(UiAnimationSurface::FloatingDock, [&](double now) {
        ++frames;
        hover.Advance(now);
        return hover.IsAnimating();
    });
    operation_feedback::Session feedback(bridge ? &scheduler : nullptr, [&] { ++flushes; });
    const ULONGLONG began = GetTickCount64();
    unsigned relevanceCalls = 0;
    ULONGLONG observedElapsed = 0;
    operation_feedback::Failure failure{"app.operation.fileFailed", L"Isolated modal hover regression",
        ERROR_ACCESS_DENIED, false, [&] {
            if (++relevanceCalls == 1) return true;
            // A TaskDialog creation failure rechecks relevance immediately.
            // Do not let its MessageBox fallback wait for a person in a test.
            observedElapsed = GetTickCount64() - began;
            ++timerCallbacks;
            observedScale = hover.FocusScale(1.28f);
            return false;
        }};
    operation_feedback::Report(failure);
    feedback.Drain(); // Real queued failure -> ShowImpl -> TaskDialogIndirect.
    Require(timerCallbacks > 0 && observedElapsed >= 100,
        "the real recoverable dialog must deliver its native timer");
    const bool hoverProgressed = frames > 0 && flushes > 0 && observedScale > 1.27f;
    Require(bridge ? hoverProgressed : !hoverProgressed && frames == 0 && observedScale == 1.0f,
        "queued error presentation must keep the Dock hover timeline moving");
    std::cout << "modal-feedback bridge=" << bridge << " frames=" << frames
        << " flushes=" << flushes << " scale-during-dialog=" << observedScale << '\n';
    scheduler.CancelAll();
}

inline void RunChild()
{
    native_drag_commit_cases::Profile profile;
    CheckDialog(false);
    CheckDialog(true);
}

inline void Run()
{
    // TaskDialog can create process-lifetime helper windows/threads. A child
    // on a never-switched desktop owns them all, so process exit releases the
    // complete native UI before the parent checks desktop cleanup.
    using native_drag_commit_cases::Require;
    native_drag_commit_cases::Profile profile;
    std::wstring name = L"SnowDesktopModalFeedback-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HDESK desktop = nullptr;
    DWORD createError = ERROR_SUCCESS, restoreError = ERROR_SUCCESS;
    std::thread([&] {
        const HDESK original = GetThreadDesktop(GetCurrentThreadId());
        desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop) createError = GetLastError();
        // CreateDesktop assigns its creator's thread. Restore explicitly;
        // CloseDesktop rejects a handle still assigned to one of our threads.
        if (!original) restoreError = ERROR_INVALID_HANDLE;
        else if (!SetThreadDesktop(original)) restoreError = GetLastError();
    }).join();
    Require(desktop != nullptr, "create private modal-feedback desktop: " + std::to_string(createError));
    struct NativeOwners
    {
        HDESK desktop;
        HANDLE output = INVALID_HANDLE_VALUE;
        PROCESS_INFORMATION child{};
        ~NativeOwners()
        {
            if (child.hProcess)
            {
                if (WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT)
                {
                    TerminateProcess(child.hProcess, 99); // Only this test's child.
                    WaitForSingleObject(child.hProcess, 5000);
                }
                CloseHandle(child.hThread);
                CloseHandle(child.hProcess);
            }
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (desktop) CloseDesktop(desktop);
        }
    } owners{desktop};
    Require(restoreError == ERROR_SUCCESS, "restore desktop creator before native dialog child: " +
        std::to_string(restoreError));
    const auto log = profile.root / L"modal-feedback-child.log";
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    owners.output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(owners.output != INVALID_HANDLE_VALUE, "open isolated native dialog evidence");
    wchar_t executable[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Require(executableLength > 0 && executableLength < MAX_PATH,
        "resolve native dialog regression executable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --modal-feedback-child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = name.data();
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = owners.output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &owners.child);
    const DWORD processError = created ? ERROR_SUCCESS : GetLastError();
    Require(created != FALSE, "start the native dialog child on its private desktop: " +
        std::to_string(processError));
    const DWORD waited = WaitForSingleObject(owners.child.hProcess, 15000);
    Require(waited == WAIT_OBJECT_0, "native dialog regression finishes within its bound");
    DWORD code = 99;
    Require(GetExitCodeProcess(owners.child.hProcess, &code) != FALSE,
        "read native dialog child result");
    CloseHandle(owners.child.hThread); owners.child.hThread = nullptr;
    CloseHandle(owners.child.hProcess); owners.child.hProcess = nullptr;
    CloseHandle(owners.output); owners.output = INVALID_HANDLE_VALUE;
    std::cout << native_drag_commit_cases::Read(log);
    Require(code == 0, "real error-dialog hover regression must pass in its child");
    Require(CloseDesktop(owners.desktop) != FALSE, "release the private modal-feedback desktop");
    owners.desktop = nullptr;
}
}
