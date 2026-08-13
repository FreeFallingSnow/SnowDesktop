#pragma once

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace snowdesktop
{

/**
 * @brief Dedicated STA worker for interactive Shell path launches.
 *
 * Enqueue only copies the launch request. ShellExecuteExW runs off the desktop
 * UI thread, so a slow shortcut resolver, DDE server, or execution delegate
 * cannot prevent pointer and foreground messages from being dispatched.
 */
class ShellLaunchWorker
{
public:
    using Executor = std::function<bool(
        HWND, const std::wstring&, int)>;

    ShellLaunchWorker();
    explicit ShellLaunchWorker(Executor executor);
    ~ShellLaunchWorker();

    ShellLaunchWorker(const ShellLaunchWorker&) = delete;
    ShellLaunchWorker& operator=(const ShellLaunchWorker&) = delete;

    /** @return Whether a copy of the launch request was accepted. */
    bool Enqueue(
        HWND owner,
        std::wstring path,
        int showCommand = SW_SHOWNORMAL);

    /**
     * @brief Stop accepting launches and discard requests not yet executing.
     *
     * An already-blocked Shell handler is allowed to finish on its detached
     * worker state so application shutdown never waits for third-party code.
     */
    void Stop();

    /** @brief Execute one launch synchronously on the calling worker STA. */
    static bool Execute(
        HWND owner,
        const std::wstring& path,
        int showCommand = SW_SHOWNORMAL);

private:
    struct Task
    {
        HWND owner = nullptr;
        std::wstring path;
        int showCommand = SW_SHOWNORMAL;
    };

    struct State
    {
        explicit State(Executor execute)
            : executor(std::move(execute)) {}

        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Task> tasks;
        Executor executor;
        bool executing = false;
        bool stopping = false;
    };

    static void Run(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
    std::thread thread_;
};

} // namespace snowdesktop
