#pragma once

#include <windows.h>
#include <shellapi.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace snowdesktop
{

/** @brief One path-based Shell operation executed by the worker. */
struct ShellFileOperationStep
{
    UINT function = 0;
    std::vector<std::wstring> sources;
    std::wstring destination;
    FILEOP_FLAGS flags = 0;
};

struct ShellFileOperationRequest
{
    std::vector<ShellFileOperationStep> steps;
};

/**
 * @brief Serial STA worker for Shell copy, move, and delete operations.
 *
 * SHFileOperationW owns and pumps its progress UI on this worker thread. It is
 * deliberately called without a desktop-window owner so a modal progress or
 * confirmation window cannot disable SnowDesktop's full-screen input surface.
 */
class ShellFileOperationWorker
{
public:
    using Completion = std::function<void(bool)>;

    ShellFileOperationWorker() = default;
    ~ShellFileOperationWorker();

    ShellFileOperationWorker(const ShellFileOperationWorker&) = delete;
    ShellFileOperationWorker& operator=(const ShellFileOperationWorker&) = delete;

    bool Enqueue(ShellFileOperationRequest request, Completion completion);
    void Stop();

    /** @brief Execute a request synchronously on the calling STA. */
    static bool Execute(const ShellFileOperationRequest& request);

private:
    struct Task
    {
        ShellFileOperationRequest request;
        Completion completion;
    };

    void Run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    bool started_ = false;
    bool stopping_ = false;
};

} // namespace snowdesktop
