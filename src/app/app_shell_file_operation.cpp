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

    snowdesktop::ShellFileOperationRequest request;
    request.steps = std::move(steps);
    return shellFileOperationWorker_.Enqueue(
        std::move(request),
        [completionWindow, completion = std::move(completion)](
            bool succeeded) mutable {
            auto* result = new (std::nothrow)
                ShellFileOperationUiCompletion{
                    succeeded, std::move(completion) };
            if (!result)
                return;
            if (!PostMessageW(
                    completionWindow,
                    kShellFileOperationCompletedMessage,
                    0,
                    reinterpret_cast<LPARAM>(result)))
                delete result;
        });
}

void DesktopApp::OnShellFileOperationCompleted(LPARAM lParam)
{
    std::unique_ptr<ShellFileOperationUiCompletion> result(
        reinterpret_cast<ShellFileOperationUiCompletion*>(lParam));
    if (!result || exitRequested_)
        return;
    if (result->callback)
        result->callback(result->succeeded);
}

void DesktopApp::StopShellFileOperationWorker()
{
    shellFileOperationWorker_.Stop();

    const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
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
