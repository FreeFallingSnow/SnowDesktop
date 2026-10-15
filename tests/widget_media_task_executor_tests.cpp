#include "widget/tasks/widget_media_task_executor.h"
#include "widget/runtime/widget_media_session_registry.h"
#include "system/controls/system_control_windows.h"

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
using snowdesktop::widget_runtime::WidgetMediaTaskRequest;
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
    Check(WidgetMediaTaskExecutor::SupportsAction("media.play") &&
            WidgetMediaTaskExecutor::SupportsAction("media.pause") &&
            WidgetMediaTaskExecutor::SupportsAction("media.toggle") &&
            WidgetMediaTaskExecutor::SupportsAction("media.stop") &&
            WidgetMediaTaskExecutor::SupportsAction("media.next") &&
            WidgetMediaTaskExecutor::SupportsAction("media.previous") &&
            WidgetMediaTaskExecutor::SupportsAction("media.seek") &&
            WidgetMediaTaskExecutor::SupportsAction("media.setRate") &&
            WidgetMediaTaskExecutor::SupportsAction("media.setShuffle") &&
            WidgetMediaTaskExecutor::SupportsAction("media.setRepeat") &&
            !WidgetMediaTaskExecutor::SupportsAction("media.unknown"),
        "the executor must expose the complete bounded media action set");

    Check(WidgetMediaTaskExecutor::ValidateRequest(
            { .action = "media.play" }) &&
            WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.seek", .positionMs = 1234 }) &&
            WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.setRate", .rate = 1.25 }) &&
            WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.setShuffle", .shuffle = false }) &&
            WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.setRepeat", .repeatMode = "list" }) &&
            !WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.seek" }) &&
            !WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.play", .positionMs = 0 }) &&
            !WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.setRate", .rate = 0.0 }) &&
            !WidgetMediaTaskExecutor::ValidateRequest(
                { .action = "media.setRepeat", .repeatMode = "all" }),
        "media action payloads must be typed and action-specific");

    WidgetMediaTaskExecutor executor(
        [](const WidgetMediaTaskRequest& request, const snowdesktop::system_control::Cancellation&)
            -> WidgetMediaTaskRunResult {
            if (request.action == "media.next" &&
                request.sessionId == "media-session-7")
                return { true, {} };
            return { false, "actionRejected" };
        });
    Check(!executor.Start(0, { .action = "media.next" }) &&
            !executor.Start(1, { .action = "media.seek" }) &&
            executor.Start(1, { .action = "media.next",
                .sessionId = "media-session-7" }) &&
            !executor.Start(1, { .action = "media.next" }),
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
        [&](const WidgetMediaTaskRequest&, const snowdesktop::system_control::Cancellation&) -> WidgetMediaTaskRunResult {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
            return { true, {} };
        });
    Check(executor.Start(9, { .action = "media.toggle" }),
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

void TestSessionObjectIdentity()
{
    using namespace snowdesktop::widget_runtime;
    struct Player { std::string app = "same-player"; unsigned plays = 0; };
    auto first = std::make_shared<Player>(), second = std::make_shared<Player>();
    MediaSessionRegistry<std::shared_ptr<Player>> registry;
    auto inventory = registry.Reconcile({first, second});
    const auto firstId = inventory[0].id, secondId = inventory[1].id;
    const auto dispatch = [&](std::string_view id) {
        if (const auto player = ResolveMediaSessionToken(inventory, id)) { ++player->plays; return true; }
        return false;
    };
    inventory = registry.Reconcile({second, first});
    Check(dispatch(firstId) && first->plays == 1 && second->plays == 0 && inventory[0].id == secondId,
        "changing current-session order cannot redirect a retained token to another player from the same app");
    inventory = registry.Reconcile({second});
    Check(!dispatch(firstId) && dispatch(secondId) && second->plays == 1,
        "retiring one session makes its token unavailable without retiring the other session");
    auto replacement = std::make_shared<Player>();
    inventory = registry.Reconcile({replacement, second});
    Check(!dispatch(firstId) && inventory[0].id != firstId && replacement->plays == 0,
        "a replacement with the same application identity cannot inherit a retired media token");
}

void TestRunningOperationCancellationAndShutdown()
{
    using snowdesktop::system_control::Cancellation;
    struct Operation
    {
        std::atomic<unsigned>* canceled;
        winrt::Windows::Foundation::AsyncStatus Status() const { return winrt::Windows::Foundation::AsyncStatus::Started; }
        void Cancel() const { ++*canceled; }
        bool GetResults() const { Check(false, "a canceled pending operation cannot request results"); return false; }
    };
    std::atomic<unsigned> entered{0}, canceled{0};
    auto executor = std::make_unique<WidgetMediaTaskExecutor>(
        [&](const WidgetMediaTaskRequest& request, const Cancellation& cancellation) -> WidgetMediaTaskRunResult {
            if (request.action == "media.next") return {true, {}};
            ++entered;
            try { return {snowdesktop::system_control::windows::Await(Operation{&canceled}, cancellation), {}}; }
            catch (...) { return {false, cancellation.Failure().error}; }
        });
    Check(executor->Start(20, {.action="media.play"}) && WaitUntil([&] { return entered.load() == 1; }),
        "the actual executor enters an unfinished WinRT-shaped operation");
    Check(executor->Cancel(20) && executor->Start(21, {.action="media.next"}), "canceling a running operation accepts independent queued work");
    std::vector<snowdesktop::widget_runtime::WidgetMediaTaskCompletion> completions;
    Check(WaitUntil([&] { auto values = executor->DrainCompletions(); completions.insert(completions.end(), values.begin(), values.end()); return completions.size() == 2; }),
        "cancellation interrupts the operation and lets the executor advance without an external completion");
    Check(canceled.load() == 1 && completions[0].error == "canceled" && completions[1].accepted,
        "the underlying operation receives Cancel while the next task retains its result");
    Check(executor->Start(22, {.action="media.play"}) && WaitUntil([&] { return entered.load() == 2; }), "shutdown fixture enters the same unfinished operation");
    const auto started = std::chrono::steady_clock::now();
    executor.reset();
    Check(std::chrono::steady_clock::now() - started < 500ms && canceled.load() == 2,
        "executor destruction cancels its active operation and joins without waiting for player completion");
    Cancellation deadline{std::make_shared<std::atomic_bool>(false), std::chrono::steady_clock::now() + 40ms};
    try { snowdesktop::system_control::windows::Await(Operation{&canceled}, deadline); Check(false, "a pending operation must reach its deadline"); }
    catch (...) { Check(deadline.Expired() && canceled.load() == 3, "deadline expiry cancels the pending operation without GetResults"); }
}
}

int main()
{
    TestActionValidationAndCompletion();
    TestCancellationOverridesLateResult();
    TestSessionObjectIdentity();
    TestRunningOperationCancellationAndShutdown();
    std::cout << "widget media task executor tests passed\n";
    return 0;
}
