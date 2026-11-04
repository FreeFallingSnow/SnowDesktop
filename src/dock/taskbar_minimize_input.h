#pragma once

#include <windows.h>
#include <atomic>
#include <future>
#include <span>
#include <thread>

namespace snowdesktop::dock
{
enum class MinimizeInputKind : DWORD { None, Pointer, Keyboard };

struct MinimizeInput
{
    MinimizeInputKind kind = MinimizeInputKind::None;
    DWORD time = 0;
    POINT point{};
    HWND foreground = nullptr;

    bool Recent(DWORD now) const noexcept
    {
        return kind != MinimizeInputKind::None && now - time <= 1500;
    }
};

// Explorer's modern input dispatch need not expose clicks to WH_GETMESSAGE.
// Observe discrete input before dispatch on a dedicated host thread. Callbacks
// publish only a small atomic snapshot (no keys/text, message dispatch or logs).
class TaskbarMinimizeInput
{
public:
    struct Api
    {
        decltype(&SetWindowsHookExW) install = &SetWindowsHookExW;
        decltype(&UnhookWindowsHookEx) uninstall = &UnhookWindowsHookEx;
        decltype(&WindowFromPoint) windowAt = &WindowFromPoint;
        decltype(&GetForegroundWindow) foreground = &GetForegroundWindow;
    };

    TaskbarMinimizeInput() = default;
    explicit TaskbarMinimizeInput(Api api) : api_(api) {}
    ~TaskbarMinimizeInput() { Stop(); }
    TaskbarMinimizeInput(const TaskbarMinimizeInput&) = delete;
    TaskbarMinimizeInput& operator=(const TaskbarMinimizeInput&) = delete;

    bool Running() const noexcept { return running_.load(); }

    DWORD Start()
    {
        if (Running()) return ERROR_SUCCESS;
        Stop();
        TaskbarMinimizeInput* expected = nullptr;
        if (!active_.compare_exchange_strong(expected, this)) return ERROR_ALREADY_EXISTS;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_)
        {
            const DWORD error = GetLastError();
            active_.store(nullptr);
            return error ? error : ERROR_NOT_ENOUGH_MEMORY;
        }
        std::promise<DWORD> started;
        auto ready = started.get_future();
        try
        {
            worker_ = std::thread([this, started = std::move(started)]() mutable {
                MSG message{};
                PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
                const HINSTANCE instance = GetModuleHandleW(nullptr);
                const HHOOK mouse = api_.install(WH_MOUSE_LL, MouseProc, instance, 0);
                DWORD error = mouse ? ERROR_SUCCESS : GetLastError();
                const HHOOK keyboard = mouse ? api_.install(WH_KEYBOARD_LL, KeyboardProc, instance, 0) : nullptr;
                if (mouse && !keyboard) error = GetLastError();
                if (!mouse || !keyboard)
                {
                    if (mouse) api_.uninstall(mouse);
                    started.set_value(error ? error : ERROR_DLL_INIT_FAILED);
                    return;
                }
                running_.store(true);
                started.set_value(ERROR_SUCCESS);
                while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT &&
                    MsgWaitForMultipleObjects(1, &stop_, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
                {
                    while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT &&
                        PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        if (message.message == WM_QUIT) break;
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (message.message == WM_QUIT) break;
                }
                api_.uninstall(keyboard);
                api_.uninstall(mouse);
                running_.store(false);
            });
        }
        catch (...)
        {
            Stop();
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        const DWORD error = ready.get();
        if (error) Stop();
        return error;
    }

    void Stop()
    {
        if (worker_.joinable())
        {
            SetEvent(stop_);
            worker_.join();
        }
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
        running_.store(false);
        auto* expected = this;
        active_.compare_exchange_strong(expected, nullptr);
        // No callbacks remain when resetting the published event.
        Publish({});
    }

    MinimizeInput Read() const noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const DWORD before = sequence_.load();
            if (before & 1) continue;
            MinimizeInput result{kind_.load(), time_.load(),
                {x_.load(), y_.load()}, foreground_.load()};
            if (sequence_.load() == before) return result;
        }
        return {};
    }

    HWND Recipient(const MinimizeInput& input) const noexcept
    {
        return input.kind == MinimizeInputKind::Pointer ? api_.windowAt(input.point) : input.foreground;
    }

    static bool BelongsToTaskbar(HWND recipient, std::span<const HWND> taskbars) noexcept
    {
        if (!recipient) return false;
        for (const HWND taskbar : taskbars)
            if (taskbar && (recipient == taskbar || IsChild(taskbar, recipient))) return true;
        return false;
    }

private:
    void Publish(const MinimizeInput& input) noexcept
    {
        sequence_.fetch_add(1);
        kind_.store(input.kind); time_.store(input.time);
        x_.store(input.point.x); y_.store(input.point.y); foreground_.store(input.foreground);
        sequence_.fetch_add(1);
    }

    static LRESULT CALLBACK MouseProc(int code, WPARAM message, LPARAM data)
    {
        if (code == HC_ACTION && data)
            if (auto* self = active_.load())
                switch (message)
                {
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                {
                    const auto& input = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
                    self->Publish({MinimizeInputKind::Pointer, input.time, input.pt, nullptr});
                    break;
                }
                }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static LRESULT CALLBACK KeyboardProc(int code, WPARAM message, LPARAM data)
    {
        if (code == HC_ACTION && data && (message == WM_KEYDOWN || message == WM_KEYUP ||
            message == WM_SYSKEYDOWN || message == WM_SYSKEYUP))
            if (auto* self = active_.load())
            {
                const auto& input = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
                // A constant-time foreground handle query; never ask another
                // UI thread to process a message from an input callback.
                self->Publish({MinimizeInputKind::Keyboard, input.time, {}, self->api_.foreground()});
            }
        return CallNextHookEx(nullptr, code, message, data);
    }

    Api api_;
    HANDLE stop_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<DWORD> sequence_{0}, time_{0};
    std::atomic<MinimizeInputKind> kind_{MinimizeInputKind::None};
    std::atomic<LONG> x_{0}, y_{0};
    std::atomic<HWND> foreground_{nullptr};
    inline static std::atomic<TaskbarMinimizeInput*> active_{nullptr};
};
}
