#pragma once

#include <windows.h>
#include <atomic>
#include <future>
#include <thread>

namespace snowdesktop
{
// WH_MOUSE_LL calls back on its installing thread. Never install it on the
// application UI thread: slow Shell extensions or rendering would stall input
// system-wide. Callbacks here must only publish atomics/asynchronous messages.
// Start/Stop are serialized by the owning UI thread.
class LowLevelMouseHook
{
public:
    struct Api
    {
        decltype(&SetWindowsHookExW) install = &SetWindowsHookExW;
        decltype(&UnhookWindowsHookEx) uninstall = &UnhookWindowsHookEx;
        decltype(&MsgWaitForMultipleObjects) waitForInput = &MsgWaitForMultipleObjects;
    };

    LowLevelMouseHook() = default;
    explicit LowLevelMouseHook(Api api) : api_(api) {}
    ~LowLevelMouseHook() { Stop(); }
    LowLevelMouseHook(const LowLevelMouseHook&) = delete;
    LowLevelMouseHook& operator=(const LowLevelMouseHook&) = delete;

    explicit operator bool() const noexcept { return running_.load(); }

    bool Start(HINSTANCE instance, HOOKPROC callback,
        bool (*initialize)() = nullptr, void (*shutdown)() = nullptr)
    {
        if (running_.load())
            return true;
        Stop();
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent_)
            return false;
        std::promise<bool> started;
        auto ready = started.get_future();
        try
        {
            thread_ = std::thread([this, instance, callback, initialize, shutdown,
                started = std::move(started)]() mutable {
                MSG message{};
                PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
                const bool initialized = !initialize || initialize();
                const HHOOK hook = initialized ? api_.install(
                    WH_MOUSE_LL, callback, instance, 0) : nullptr;
                running_.store(hook != nullptr);
                started.set_value(hook != nullptr);
                if (!hook)
                {
                    if (initialized && shutdown) shutdown();
                    return;
                }

                bool quit = false;
                // A system/input wake can win over the stop event. Check stop
                // before every outer wait as well as while draining messages,
                // otherwise shutdown can keep re-entering the message branch.
                while (!quit &&
                    WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT &&
                    api_.waitForInput(
                    1, &stopEvent_, FALSE, INFINITE, QS_ALLINPUT) ==
                        WAIT_OBJECT_0 + 1)
                {
                    while (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT &&
                        PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        if (message.message == WM_QUIT)
                        {
                            quit = true;
                            break;
                        }
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                api_.uninstall(hook);
                if (shutdown) shutdown();
                running_.store(false);
            });
        }
        catch (...)
        {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
            return false;
        }
        if (ready.get())
            return true;
        Stop();
        return false;
    }

    void Stop()
    {
        if (thread_.joinable())
        {
            SetEvent(stopEvent_);
            thread_.join();
        }
        if (stopEvent_)
        {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
        running_.store(false);
    }

private:
    Api api_;
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> running_{false};
};
}
