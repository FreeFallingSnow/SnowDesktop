#pragma once

#include <windows.h>
#include <shlobj.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace snowdesktop
{

/**
 * @brief Dedicated STA worker for interactive Shell path launches.
 *
 * Enqueue copies the launch request and, for Shell items, its absolute PIDL.
 * The default executor dispatches a supervised helper process per request.
 * A slow Shell handler therefore cannot hold the desktop's loader lock or
 * leave subsequent launches queued behind it. Custom executors run on the STA.
 */
class ShellLaunchWorker
{
public:
    using Executor = std::function<bool(
        HWND, const std::wstring&, PCIDLIST_ABSOLUTE, int)>;

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
     * @brief Queue a Shell item activation using a private copy of its PIDL.
     *
     * Shortcut activation uses IContextMenu inside the helper process. The
     * path remains available as a compatibility fallback.
     */
    bool EnqueueShellItem(
        HWND owner,
        std::wstring path,
        PCIDLIST_ABSOLUTE absolutePidl,
        int showCommand = SW_SHOWNORMAL);

    /**
     * @brief Stop accepting launches and discard requests not yet executing.
     *
     * An already-blocked Shell handler is allowed to finish on its detached
     * worker state so application shutdown never waits for third-party code.
     */
    void Stop();

    /** @brief Dispatch one isolated launch; true means the helper was started. */
    static bool Execute(
        HWND owner,
        const std::wstring& path,
        PCIDLIST_ABSOLUTE absolutePidl,
        int showCommand = SW_SHOWNORMAL);

    /**
     * @brief Dispatch a user-initiated Open and its shortcut elevation policy.
     *
     * Foreground eligibility is handed to a private helper STA. Shell/DDE
     * completion is synchronous there and never waited for on the desktop.
     */
    static bool ExecuteInteractive(
        HWND owner,
        const std::wstring& path,
        PCIDLIST_ABSOLUTE absolutePidl,
        int showCommand = SW_SHOWNORMAL);

    /** @brief Dispatch one explicit runas launch in an isolated helper. */
    static bool ExecuteRunAsAdministrator(
        HWND owner,
        const std::wstring& path,
        PCIDLIST_ABSOLUTE absolutePidl,
        int showCommand = SW_SHOWNORMAL);

    /** @brief Whether a shortcut or its target requests administrator launch. */
    static bool ShortcutRequestsAdministrator(
        const std::wstring& path);

private:
    struct Task
    {
        HWND owner = nullptr;
        std::wstring path;
        std::vector<unsigned char> absolutePidl;
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

    bool EnqueueTask(Task task);
    static void Run(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
    std::thread thread_;
};

} // namespace snowdesktop
