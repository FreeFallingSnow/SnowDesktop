#include "shell_launch_worker.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <utility>

namespace snowdesktop
{

ShellLaunchWorker::ShellLaunchWorker()
    : ShellLaunchWorker(&ShellLaunchWorker::Execute)
{
}

ShellLaunchWorker::ShellLaunchWorker(Executor executor)
    : state_(std::make_shared<State>(std::move(executor)))
{
}

ShellLaunchWorker::~ShellLaunchWorker()
{
    Stop();
}

bool ShellLaunchWorker::Enqueue(
    HWND owner,
    std::wstring path,
    int showCommand)
{
    if (path.empty() || !state_ || !state_->executor)
        return false;

    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopping)
            return false;
        try
        {
            state->tasks.push_back(
                { owner, std::move(path), showCommand });
            if (!thread_.joinable())
                thread_ = std::thread(&ShellLaunchWorker::Run, state);
        }
        catch (...)
        {
            if (!state->tasks.empty())
                state->tasks.pop_back();
            return false;
        }
    }
    state->cv.notify_one();
    return true;
}

void ShellLaunchWorker::Stop()
{
    if (!state_)
        return;

    auto state = state_;
    bool joinWorker = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->stopping)
        {
            state->stopping = true;
            state->tasks.clear();
        }
        joinWorker = !state->executing;
    }
    state->cv.notify_all();

    if (!thread_.joinable())
        return;
    if (joinWorker)
        thread_.join();
    else
        thread_.detach();
}

bool ShellLaunchWorker::Execute(
    HWND owner,
    const std::wstring& path,
    int showCommand)
{
    if (path.empty())
        return false;

    constexpr ULONG launchMask =
        SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
    if (path.size() >= 6 &&
        _wcsnicmp(path.c_str(), L"shell:", 6) == 0)
    {
        PIDLIST_ABSOLUTE rawPidl = nullptr;
        const HRESULT parseResult = SHParseDisplayName(
            path.c_str(), nullptr, &rawPidl, 0, nullptr);
        if (SUCCEEDED(parseResult) && rawPidl)
        {
            SHELLEXECUTEINFOW namespaceExecuteInfo{};
            namespaceExecuteInfo.cbSize = sizeof(namespaceExecuteInfo);
            namespaceExecuteInfo.fMask = launchMask | SEE_MASK_IDLIST;
            namespaceExecuteInfo.hwnd =
                owner && IsWindow(owner) ? owner : nullptr;
            namespaceExecuteInfo.lpIDList = rawPidl;
            namespaceExecuteInfo.nShow = showCommand;
            const bool opened =
                ShellExecuteExW(&namespaceExecuteInfo) != FALSE;
            CoTaskMemFree(rawPidl);
            if (opened)
                return true;
        }
        else if (rawPidl)
        {
            CoTaskMemFree(rawPidl);
        }
    }

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    // This thread has no message pump. Complete DDE and execution-delegate
    // handoffs here instead of borrowing the desktop UI thread's message pump.
    executeInfo.fMask = launchMask;
    executeInfo.hwnd = owner && IsWindow(owner) ? owner : nullptr;
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = path.c_str();
    executeInfo.nShow = showCommand;
    return ShellExecuteExW(&executeInfo) != FALSE;
}

void ShellLaunchWorker::Run(const std::shared_ptr<State>& state)
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult))
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->tasks.clear();
        state->stopping = true;
        return;
    }

    for (;;)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&] {
                return state->stopping || !state->tasks.empty();
            });
            if (state->stopping && state->tasks.empty())
                break;
            task = std::move(state->tasks.front());
            state->tasks.pop_front();
            state->executing = true;
        }

        try
        {
            state->executor(
                task.owner, task.path, task.showCommand);
        }
        catch (...)
        {
            // A failing injected/custom executor must not terminate the
            // worker or discard subsequent user launch requests.
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->executing = false;
        }
    }

    CoUninitialize();
}

} // namespace snowdesktop
