#include "shell_file_operation_worker.h"

#include <objbase.h>
#include <shellapi.h>

#include <utility>

namespace snowdesktop
{
namespace
{

std::wstring BuildPathList(const std::vector<std::wstring>& paths)
{
    std::wstring result;
    for (const auto& path : paths)
    {
        if (path.empty())
            continue;
        result.append(path);
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

std::wstring BuildDestination(const std::wstring& destination)
{
    if (destination.empty())
        return {};
    std::wstring result = destination;
    result.push_back(L'\0');
    result.push_back(L'\0');
    return result;
}

} // namespace

ShellFileOperationWorker::~ShellFileOperationWorker()
{
    Stop();
}

bool ShellFileOperationWorker::Enqueue(
    ShellFileOperationRequest request, Completion completion)
{
    if (request.steps.empty())
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return false;
        tasks_.push_back({ std::move(request), std::move(completion) });
        if (!started_)
        {
            try
            {
                thread_ = std::thread(&ShellFileOperationWorker::Run, this);
                started_ = true;
            }
            catch (...)
            {
                tasks_.pop_back();
                return false;
            }
        }
    }
    cv_.notify_one();
    return true;
}

void ShellFileOperationWorker::Stop()
{
    std::deque<Task> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        cancelled.swap(tasks_);
    }

    for (auto& task : cancelled)
        if (task.completion)
            task.completion(false);

    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool ShellFileOperationWorker::Execute(
    const ShellFileOperationRequest& request)
{
    bool anySucceeded = false;
    for (const auto& step : request.steps)
    {
        if (step.function == 0 || step.sources.empty())
            continue;

        const std::wstring sources = BuildPathList(step.sources);
        const std::wstring destination =
            BuildDestination(step.destination);
        SHFILEOPSTRUCTW operation{};
        operation.hwnd = nullptr;
        operation.wFunc = step.function;
        operation.pFrom = sources.c_str();
        operation.pTo = destination.empty()
            ? nullptr : destination.c_str();
        operation.fFlags = step.flags;

        const int result = SHFileOperationW(&operation);
        if (result == 0 && !operation.fAnyOperationsAborted)
            anySucceeded = true;
    }
    return anySucceeded;
}

void ShellFileOperationWorker::Run()
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    for (;;)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (stopping_ && tasks_.empty())
                break;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        const bool succeeded = Execute(task.request);
        if (task.completion)
            task.completion(succeeded);
    }

    if (SUCCEEDED(comResult))
        CoUninitialize();
}

} // namespace snowdesktop
