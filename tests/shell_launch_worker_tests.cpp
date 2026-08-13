#include "shell_launch_worker.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

struct BlockingExecutorState
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::wstring> paths;
    std::thread::id executionThread;
    bool releaseFirst = false;
    int finished = 0;
};

bool WaitForPathCount(
    const std::shared_ptr<BlockingExecutorState>& state,
    size_t expected)
{
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(
        lock, std::chrono::seconds(5), [&] {
            return state->paths.size() >= expected;
        });
}

void TestLaunchesAreCopiedAndRunOffTheCallerThread()
{
    auto state = std::make_shared<BlockingExecutorState>();
    snowdesktop::ShellLaunchWorker worker(
        [state](HWND, const std::wstring& path, int) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->executionThread = std::this_thread::get_id();
            state->paths.push_back(path);
            state->cv.notify_all();
            if (state->paths.size() == 1)
            {
                state->cv.wait(lock, [&] {
                    return state->releaseFirst;
                });
            }
            ++state->finished;
            state->cv.notify_all();
            return true;
        });

    const std::thread::id callerThread = std::this_thread::get_id();
    std::wstring firstPath = L"first.lnk";
    Check(
        worker.Enqueue(nullptr, firstPath),
        "the first launch request must be accepted");
    Check(
        WaitForPathCount(state, 1),
        "the first launch request must reach the worker");

    firstPath.assign(L"mutated-after-enqueue.lnk");
    Check(
        worker.Enqueue(nullptr, L"second.txt"),
        "a producer must remain responsive while the worker is blocked");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->releaseFirst = true;
    }
    state->cv.notify_all();
    Check(
        WaitForPathCount(state, 2),
        "the queued launch must run after the blocked launch completes");

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(
            lock, std::chrono::seconds(5), [&] {
                return state->finished == 2;
            });
        Check(
            state->paths.size() == 2 &&
                state->paths[0] == L"first.lnk" &&
                state->paths[1] == L"second.txt",
            "the worker must preserve copied paths and FIFO ordering");
        Check(
            state->executionThread != callerThread,
            "Shell execution must not run on the enqueueing UI thread");
    }
    worker.Stop();
}

void TestStopDoesNotJoinABlockedShellHandler()
{
    auto state = std::make_shared<BlockingExecutorState>();
    snowdesktop::ShellLaunchWorker worker(
        [state](HWND, const std::wstring& path, int) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->paths.push_back(path);
            state->cv.notify_all();
            state->cv.wait(lock, [&] {
                return state->releaseFirst;
            });
            ++state->finished;
            state->cv.notify_all();
            return true;
        });

    Check(
        worker.Enqueue(nullptr, L"blocked.lnk"),
        "the blocking launch request must be accepted");
    Check(
        WaitForPathCount(state, 1),
        "the blocking launch request must start");

    const auto start = std::chrono::steady_clock::now();
    worker.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Check(
        elapsed < std::chrono::milliseconds(250),
        "shutdown must not join a Shell handler blocked in third-party code");
    Check(
        !worker.Enqueue(nullptr, L"after-stop.txt"),
        "a stopped worker must reject new launch requests");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->releaseFirst = true;
    }
    state->cv.notify_all();
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        Check(
            state->cv.wait_for(
                lock, std::chrono::seconds(5), [&] {
                    return state->finished == 1;
                }),
            "a detached in-flight launch must retain safe worker state");
    }
}

void TestInvalidRequestsAreRejected()
{
    snowdesktop::ShellLaunchWorker worker(
        [](HWND, const std::wstring&, int) {
            return true;
        });
    Check(
        !worker.Enqueue(nullptr, L""),
        "an empty launch path must be rejected");
    worker.Stop();
}

} // namespace

int main()
{
    TestLaunchesAreCopiedAndRunOffTheCallerThread();
    TestStopDoesNotJoinABlockedShellHandler();
    TestInvalidRequestsAreRejected();
    if (failures != 0)
    {
        std::cerr << failures
                  << " Shell launch worker test(s) failed\n";
        return 1;
    }
    std::cout << "All Shell launch worker tests passed\n";
    return 0;
}
