#include "widget_media_task_executor.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using snowdesktop::widget_runtime::WidgetMediaTaskExecutor;
using snowdesktop::widget_runtime::WidgetMediaTaskRunResult;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template<typename Predicate>
bool WaitUntil(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

void TestActionValidationAndCompletion()
{
    Check(WidgetMediaTaskExecutor::SupportsAction("media.toggle") &&
            WidgetMediaTaskExecutor::SupportsAction("media.next") &&
            WidgetMediaTaskExecutor::SupportsAction("media.previous") &&
            !WidgetMediaTaskExecutor::SupportsAction("media.stop"),
        "the executor must expose only implemented media actions");

    WidgetMediaTaskExecutor executor(
        [](std::string_view action) -> WidgetMediaTaskRunResult {
            if (action == "media.next") return { true, {} };
            return { false, "actionRejected" };
        });
    Check(!executor.Start(0, "media.next") &&
            !executor.Start(1, "media.stop") &&
            executor.Start(1, "media.next") &&
            !executor.Start(1, "media.next"),
        "invalid and duplicate task requests must be rejected");

    std::vector<snowdesktop::widget_runtime::WidgetMediaTaskCompletion>
        completions;
    Check(WaitUntil([&] {
            completions = executor.DrainCompletions();
            return !completions.empty();
        }),
        "the worker must publish a bounded asynchronous completion");
    Check(completions.size() == 1 && completions[0].id == 1 &&
            completions[0].accepted && completions[0].error.empty() &&
            executor.ActiveCount() == 0,
        "successful media completions must retain identity and acceptance");
}

void TestCancellationOverridesLateResult()
{
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    WidgetMediaTaskExecutor executor(
        [&](std::string_view) -> WidgetMediaTaskRunResult {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
            return { true, {} };
        });
    Check(executor.Start(9, "media.toggle"),
        "a valid media task must enter the worker queue");
    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(lock, 2s, [&] { return entered; }),
            "the fake action runner must start");
    }
    Check(executor.Cancel(9) && !executor.Cancel(100),
        "cancellation must affect only active task IDs");
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    condition.notify_all();

    std::vector<snowdesktop::widget_runtime::WidgetMediaTaskCompletion>
        completions;
    Check(WaitUntil([&] {
            completions = executor.DrainCompletions();
            return !completions.empty();
        }),
        "a canceled running action must still acknowledge completion");
    Check(completions.size() == 1 && completions[0].id == 9 &&
            !completions[0].accepted &&
            completions[0].error == "canceled",
        "cancellation must override a late successful system result");
}
}

int main()
{
    TestActionValidationAndCompletion();
    TestCancellationOverridesLateResult();
    std::cout << "widget media task executor tests passed\n";
    return 0;
}
