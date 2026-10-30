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

inline void Run()
{
    // Never show or automate the user's desktop host. Native dialogs and all
    // message-only windows live on a test-owned desktop that is never switched.
    const std::wstring name = L"SnowDesktopModalFeedback-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    native_drag_commit_cases::Require(desktop != nullptr, "create private modal-feedback desktop");
    std::exception_ptr failure;
    std::thread([&] {
        try
        {
            native_drag_commit_cases::Require(SetThreadDesktop(desktop) != FALSE,
                "bind native error-dialog thread to the private desktop");
            native_drag_commit_cases::Profile profile;
            CheckDialog(false); // Old production presentation has no bridge.
            CheckDialog(true);
        }
        catch (...) { failure = std::current_exception(); }
    }).join();
    const bool closed = CloseDesktop(desktop) != FALSE;
    if (failure) std::rethrow_exception(failure);
    native_drag_commit_cases::Require(closed, "release the private modal-feedback desktop");
}
}
