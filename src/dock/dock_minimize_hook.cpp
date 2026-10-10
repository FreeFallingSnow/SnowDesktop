#include "dock_minimize_protocol.h"
#include "dock_window_source_cloak.h"

extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopDockSourceCloakHook(
    int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && lParam)
    {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message->message >= 0xC000 &&
            message->message == snowdesktop::dock_source_cloak::CommandMessage())
        {
            const HWND receiver = reinterpret_cast<HWND>(
                GetPropW(message->hwnd, snowdesktop::dock_minimize::kTargetProperty));
            const auto owner = snowdesktop::dock_source_cloak::ReadOwner(message->hwnd);
            DWORD receiverProcess = 0;
            GetWindowThreadProcessId(receiver, &receiverProcess);
            if (snowdesktop::dock_minimize::HasLiveOwner(receiver) &&
                (!message->wParam || owner.process == ULongToHandle(receiverProcess)))
                snowdesktop::dock_source_cloak::HandleCommand(
                    message->hwnd, message->wParam, message->lParam);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Runs only in the marked application's window thread. No worker, allocation,
// capture or replacement minimize command is executed inside the target.
extern "C" __declspec(dllexport) LRESULT CALLBACK SnowDesktopDockMinimizeHook(
    int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HCBT_MINMAX ||
        !snowdesktop::dock_minimize::IsMinimizeCommand(LOWORD(lParam)))
        return CallNextHookEx(nullptr, code, wParam, lParam);

    const HWND window = reinterpret_cast<HWND>(wParam);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    const HWND receiver = reinterpret_cast<HWND>(
        GetPropW(window, snowdesktop::dock_minimize::kTargetProperty));
    static thread_local bool notifying = false;
    if (!notifying && process == GetCurrentProcessId() &&
        snowdesktop::dock_minimize::HasLiveOwner(receiver) &&
        IsWindowVisible(window) && !IsIconic(window))
    {
        notifying = true;
        const UINT message = snowdesktop::dock_minimize::RequestMessage();
        const DWORD deadline = GetTickCount() + snowdesktop::dock_minimize::kRequestTimeoutMs;
        DWORD_PTR result = 0;
        if (message && !SendMessageTimeoutW(receiver, message, wParam,
                // Allow the host's bounded source-cloak command back into this
                // thread while it prepares the image. notifying prevents a
                // reentrant minimize from creating another host request.
                static_cast<LPARAM>(deadline), SMTO_ABORTIFHUNG,
                snowdesktop::dock_minimize::kRequestTimeoutMs, &result))
        {
            const UINT cancel = snowdesktop::dock_minimize::CancelMessage();
            if (cancel)
                PostMessageW(receiver, cancel, wParam, static_cast<LPARAM>(deadline));
        }
        notifying = false;
    }
    // Never swallow the original request, even on timeout or host shutdown.
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
