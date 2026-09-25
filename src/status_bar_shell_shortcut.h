#pragma once
#include "status_bar.h"
#include "ui_animation_scheduler.h"
#include <utility>

namespace snowdesktop
{
// Resolve the click before a nested menu unwinds: Ctrl may be released while
// that happens, but it must not turn a system-panel request into our panel.
inline StatusBarAction ResolveStatusBarClick(StatusBarAction action, bool controlDown)
{
    return action == StatusBarAction::ControlCenter && controlDown ?
        StatusBarAction::SystemControlCenter : action;
}

enum class StatusBarShortcutResult { Sent, Cancelled, TimedOut, Failed };
struct StatusBarShortcutCallbacks
{
    std::function<bool(int)> keyDown;
    std::function<UINT(UINT, INPUT*, int)> send;
    std::function<bool(UiScheduleToken)> current;
    std::function<void(UiScheduleToken, StatusBarShortcutResult)> finished;
};

// Do not synthesize release/restore pairs for physically held modifiers. Wait
// asynchronously for the user's release instead. The caller cancels this same
// token on another bar action, dismissal or shutdown; the context guard also
// rejects a hidden bar, fullscreen or a changed foreground window.
inline UiScheduleToken ScheduleStatusBarShellShortcut(UiAnimationScheduler& scheduler,
    WORD key, StatusBarShortcutCallbacks callbacks, UINT timeoutMilliseconds = 5000)
{
    const double deadline = UiAnimationScheduler::MonotonicMilliseconds() + timeoutMilliseconds;
    return scheduler.ScheduleInterval(16,
        [&scheduler, key, deadline, callbacks = std::move(callbacks)](UiScheduleToken token) {
            const auto finish = [&](StatusBarShortcutResult result) {
                scheduler.Cancel(token);
                callbacks.finished(token, result);
            };
            if (!callbacks.current(token)) { finish(StatusBarShortcutResult::Cancelled); return; }
            if (UiAnimationScheduler::MonotonicMilliseconds() >= deadline)
            { finish(StatusBarShortcutResult::TimedOut); return; }
            for (const int modifier : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
                if (callbacks.keyDown(modifier)) return;
            if (callbacks.keyDown(key)) return;

            INPUT input[4]{};
            for (auto& item : input) item.type = INPUT_KEYBOARD;
            input[0].ki.wVk = VK_LWIN; input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
            input[1].ki.wVk = key;
            input[2].ki.wVk = key; input[2].ki.dwFlags = KEYEVENTF_KEYUP;
            input[3].ki.wVk = VK_LWIN; input[3].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
            const UINT sent = callbacks.send(4, input, sizeof(INPUT));
            if (sent > 0 && sent < 4)
            {
                // Best-effort release of only our unmatched synthetic presses.
                // Never retry the chord: it toggles the system surface.
                INPUT release[2]{input[2], input[3]};
                callbacks.send(sent == 2 ? 2 : 1, release + (sent == 2 ? 0 : 1), sizeof(INPUT));
            }
            finish(sent == 4 ? StatusBarShortcutResult::Sent : StatusBarShortcutResult::Failed);
        });
}
}
