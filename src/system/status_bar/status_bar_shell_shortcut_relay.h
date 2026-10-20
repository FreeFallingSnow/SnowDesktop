#pragma once

#include "status_bar_shell_shortcut.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace snowdesktop
{
// A desktop render child can share Explorer's input queue, as observed in the
// investigated hang. Moving SendInput to another thread alone does not change
// its destination: this thread must first own a separate, unowned foreground/
// focus window. No attachment or fallback to the desktop proxy is used here.
class StatusBarShellShortcutRelay
{
public:
    enum class Result { Pending, Sent, Cancelled, TimedOut, Failed };

    struct Api
    {
        std::function<HWND()> foreground = [] { return GetForegroundWindow(); };
        std::function<bool(HWND)> activate = [](HWND window) {
            if (!SetForegroundWindow(window)) return false;
            SetFocus(window);
            return true;
        };
        std::function<bool(HWND, DWORD)> focused = [](HWND window, DWORD thread) {
            GUITHREADINFO info{sizeof(info)};
            return GetGUIThreadInfo(thread, &info) && info.hwndFocus == window &&
                info.hwndActive == window;
        };
        std::function<bool(int)> keyDown = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        std::function<UINT(UINT, INPUT*, int)> send = [](UINT count, INPUT* input, int size) {
            return SendInput(count, input, size);
        };
        std::function<ULONGLONG()> nowMilliseconds = [] { return GetTickCount64(); };
    };

    static std::shared_ptr<StatusBarShellShortcutRelay> Start(HWND initialForeground,
        StatusBarShellChord chord, Api api)
    {
        if (!initialForeground || !chord.key || !api.foreground || !api.activate || !api.focused ||
            !api.keyDown || !api.send || !api.nowMilliseconds)
            return {};
        bool expected = false;
        if (!inFlight_.compare_exchange_strong(expected, true)) return {};
        try
        {
            auto state = std::make_shared<State>(initialForeground, chord, std::move(api));
            auto relay = std::shared_ptr<StatusBarShellShortcutRelay>(new StatusBarShellShortcutRelay(state));
            // Only State is retained by the worker. Releasing the UI request
            // neither joins a foreign GUI call nor leaves a DesktopApp pointer.
            std::thread([state] { Run(state); }).detach();
            return relay;
        }
        catch (...)
        {
            inFlight_.store(false);
            return {};
        }
    }
    static std::shared_ptr<StatusBarShellShortcutRelay> Start(HWND initialForeground, StatusBarShellChord chord)
    { return Start(initialForeground, chord, Api{}); }

    ~StatusBarShellShortcutRelay() { Cancel(); }
    StatusBarShellShortcutRelay(const StatusBarShellShortcutRelay&) = delete;
    StatusBarShellShortcutRelay& operator=(const StatusBarShellShortcutRelay&) = delete;

    Result Poll() const noexcept { return state_->result.load(std::memory_order_acquire); }
    UINT AcceptedCount() const noexcept { return state_->accepted.load(std::memory_order_acquire); }
    UINT SentCount() const noexcept { return AcceptedCount(); }
    bool Running() const noexcept { return state_->running.load(std::memory_order_acquire); }
    HWND Window() const noexcept { return state_->window.load(std::memory_order_acquire); }
    DWORD ThreadId() const noexcept { return state_->thread.load(std::memory_order_acquire); }

    void Cancel() noexcept
    {
        // Cancellation wins only before injection commits. In particular,
        // destroying the UI request after Poll()==Sent must not retire the
        // foreground queue while the accepted chord is still being consumed.
        unsigned expected = kPending;
        state_->gate.compare_exchange_strong(expected, kCancelled);
    }

private:
    static constexpr unsigned kPending = 0, kSending = 1, kCancelled = 2;
    static constexpr ULONGLONG kPreparationMilliseconds = 5000, kHandoffMilliseconds = 1500;
    static constexpr wchar_t kWindowClass[] = L"SnowDesktopShellShortcutRelay";

    struct State
    {
        State(HWND foreground, StatusBarShellChord keys, Api callbacks) :
            initialForeground(foreground), chord(keys), api(std::move(callbacks)) {}
        const HWND initialForeground;
        const StatusBarShellChord chord;
        const Api api;
        std::atomic<unsigned> gate{kPending};
        std::atomic<Result> result{Result::Pending};
        std::atomic<UINT> accepted{0};
        std::atomic<bool> running{true};
        std::atomic<HWND> window{nullptr};
        std::atomic<DWORD> thread{0};
    };

    explicit StatusBarShellShortcutRelay(std::shared_ptr<State> state) : state_(std::move(state)) {}

    static bool RegisterWindowClass() noexcept
    {
        static const bool registered = [] {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = kWindowClass;
            return RegisterClassW(&windowClass) != 0;
        }();
        return registered;
    }

    static bool KeysReleased(const Api& api, StatusBarShellChord chord)
    {
        // Every relay changes the foreground queue, including notification and
        // calendar chords whose old same-thread path did not require this.
        for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
            if (api.keyDown(key)) return false;
        for (const int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN, static_cast<int>(chord.key)})
            if (api.keyDown(key)) return false;
        return true;
    }

    static bool Pump() noexcept
    {
        MSG message{};
        // Bound each drain so a stream of input cannot postpone cancellation
        // or the request deadline indefinitely.
        for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count)
        {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return true;
    }

    static void WaitForInput() noexcept
    { MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE); }

    static void Run(const std::shared_ptr<State>& state) noexcept
    {
        struct Cleanup
        {
            std::shared_ptr<State> state;
            HWND window = nullptr;
            ~Cleanup()
            {
                if (window && IsWindow(window)) DestroyWindow(window);
                state->window.store(nullptr, std::memory_order_release);
                inFlight_.store(false, std::memory_order_release);
                state->running.store(false, std::memory_order_release);
            }
        } cleanup{state};

        try
        {
            const auto& api = state->api;
            const DWORD thread = GetCurrentThreadId();
            state->thread.store(thread, std::memory_order_release);
            const ULONGLONG deadline = api.nowMilliseconds() + kPreparationMilliseconds;
            if (state->gate.load() == kCancelled || api.foreground() != state->initialForeground)
            { state->result.store(Result::Cancelled); return; }
            if (!RegisterWindowClass())
            { state->result.store(Result::Failed); return; }

            cleanup.window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"",
                WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!cleanup.window)
            { state->result.store(Result::Failed); return; }
            const HWND window = cleanup.window;
            state->window.store(window, std::memory_order_release);
            ShowWindow(window, SW_SHOWNOACTIVATE);

            bool activated = false;
            for (;;)
            {
                if (!Pump() || !IsWindow(window) || state->gate.load() == kCancelled)
                { state->result.store(Result::Cancelled); return; }
                if (api.nowMilliseconds() >= deadline)
                { state->result.store(Result::TimedOut); return; }
                if (api.foreground() != (activated ? window : state->initialForeground))
                { state->result.store(Result::Cancelled); return; }
                if (!KeysReleased(api, state->chord)) { WaitForInput(); continue; }
                if (!activated)
                {
                    // This attempt is never retried. A denied foreground
                    // request must not inject into whichever window kept it.
                    if (!api.activate(window) || api.foreground() != window || !api.focused(window, thread))
                    { state->result.store(Result::Failed); return; }
                    activated = true;
                    // Activation can dispatch messages and take time. Sample
                    // physical input again instead of relying on the UI's
                    // earlier release check.
                    continue;
                }
                if (!api.focused(window, thread) || api.foreground() != window)
                { state->result.store(Result::Cancelled); return; }
                unsigned expected = kPending;
                if (!state->gate.compare_exchange_strong(expected, kSending))
                { state->result.store(Result::Cancelled); return; }
                break;
            }

            INPUT input[6]{};
            for (auto& event : input) event.type = INPUT_KEYBOARD;
            const UINT presses = state->chord.alt ? 3u : 2u;
            const UINT count = presses * 2;
            const WORD keys[]{VK_LWIN, state->chord.alt ? static_cast<WORD>(VK_MENU) : state->chord.key,
                state->chord.key};
            for (UINT index = 0; index < presses; ++index)
            {
                input[index].ki.wVk = keys[index];
                input[index].ki.dwFlags = index == 0 ? KEYEVENTF_EXTENDEDKEY : 0;
                input[count - 1 - index] = input[index];
                input[count - 1 - index].ki.dwFlags |= KEYEVENTF_KEYUP;
            }
            const UINT accepted = api.send(count, input, sizeof(INPUT));
            state->accepted.store(accepted, std::memory_order_release);
            if (accepted > 0 && accepted < count)
            {
                INPUT release[3]{};
                UINT releases = 0;
                for (UINT index = presses; index-- > 0;)
                    if (accepted > index && accepted <= count - 1 - index)
                        release[releases++] = input[count - 1 - index];
                if (releases) api.send(releases, release, sizeof(INPUT));
            }
            const ULONGLONG handoffDeadline = api.nowMilliseconds() + kHandoffMilliseconds;
            state->result.store(accepted == count ? Result::Sent : Result::Failed, std::memory_order_release);
            if (!accepted) return;

            // SendInput reports acceptance, not delivery or Shell visibility.
            // Keep this queue alive until another foreground window takes over
            // or the bounded handoff ends. Do not restore desktop focus here.
            while (IsWindow(window) && api.foreground() == window &&
                api.nowMilliseconds() < handoffDeadline && Pump())
                WaitForInput();
        }
        catch (...)
        {
            state->result.store(Result::Failed, std::memory_order_release);
        }
    }

    inline static std::atomic<bool> inFlight_{false};
    std::shared_ptr<State> state_;
};
}
