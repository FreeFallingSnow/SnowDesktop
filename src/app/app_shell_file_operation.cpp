#include "app.h"

#include <new>
#include <utility>

// Asynchronous path-based Shell file operations.

bool DesktopApp::QueueShellFileOperation(
    std::vector<snowdesktop::ShellFileOperationStep> steps,
    FileOperationCompletion completion)
{
    HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    if (!completionWindow || !IsWindow(completionWindow) || steps.empty())
        return false;

    auto* result = new (std::nothrow)
        ShellFileOperationUiCompletion{
            false, std::move(completion) };
    if (!result)
        return false;

    snowdesktop::ShellFileOperationRequest request;
    request.steps = std::move(steps);
    const bool queued = shellFileOperationWorker_.Enqueue(
        std::move(request),
        [completionWindow, result](bool succeeded) {
            result->succeeded = succeeded;
            if (!PostMessageW(
                    completionWindow,
                    kShellFileOperationCompletedMessage,
                    0,
                    reinterpret_cast<LPARAM>(result)))
                delete result;
        });
    if (!queued)
    {
        delete result;
        return false;
    }
    ++shellFileOperationInFlight_;
    ApplyFloatingDockLayerPolicy();
    return true;
}

void DesktopApp::OnShellFileOperationCompleted(LPARAM lParam)
{
    std::unique_ptr<ShellFileOperationUiCompletion> result(
        reinterpret_cast<ShellFileOperationUiCompletion*>(lParam));
    if (!result || exitRequested_)
        return;
    if (result->callback)
        result->callback(result->succeeded);
    if (shellFileOperationInFlight_ > 0)
        --shellFileOperationInFlight_;
    // SHFileOperationW can promote the Explorer/foreground window while the
    // worker thread owns its progress UI. Refresh callbacks then rebuild the
    // desktop tree, so restore the floating Dock layer only after they ran.
    RestoreDesktopWindowLayer();
    if (snowdesktop::floating_dock_rules::
            ShouldRefocusFloatingDockKeyboardSession(
                floatingDockVisible_,
                floatingDockKeyboardSessionActive_,
                shellFileOperationInFlight_,
                shellPopupMenuLayerDepth_))
        RefocusFloatingDockKeyboardSession();
}

void DesktopApp::StopShellFileOperationWorker()
{
    shellFileOperationWorker_.Stop();

    const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    // The worker Stop() path runs queued completions without going through
    // OnShellFileOperationCompleted, so clear the UI-side in-flight state
    // even when the completion window is already gone during shutdown.
    shellFileOperationInFlight_ = 0;
    if (!completionWindow || !IsWindow(completionWindow))
        return;

    MSG message{};
    while (PeekMessageW(
        &message, completionWindow,
        kShellFileOperationCompletedMessage,
        kShellFileOperationCompletedMessage,
        PM_REMOVE))
    {
        delete reinterpret_cast<ShellFileOperationUiCompletion*>(
            message.lParam);
    }
}
