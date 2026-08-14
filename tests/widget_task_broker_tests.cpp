#include "widget_task_broker.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using snowdesktop::widget_runtime::TaskBrokerActionType;
using snowdesktop::widget_runtime::TaskBrokerCancelReason;
using snowdesktop::widget_runtime::TaskDescriptor;
using snowdesktop::widget_runtime::TaskStartOptions;
using snowdesktop::widget_runtime::WidgetTaskBroker;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestRegistrationAndStartGuards()
{
    WidgetTaskBroker broker;
    std::string error;
    Check(!broker.RegisterTask({}, error) && !error.empty(),
        "tasks without a name must be rejected");
    Check(!broker.RegisterTask({ "bad", {}, false, 0 }, error),
        "tasks require a valid per-instance concurrency limit");
    Check(broker.RegisterTask(
            { "media.play", "media.action", true, 1 }, error) &&
            !broker.RegisterTask(
                { "media.play", "media.action", true, 1 }, error),
        "task descriptors must register once");

    TaskStartOptions options;
    Check(!broker.Start("widget", "missing", options) &&
            !broker.Start("widget", "media.play", options),
        "unknown and unauthorized tasks must be rejected");
    options.permissionGranted = true;
    Check(!broker.Start("widget", "media.play", options),
        "gesture-required tasks must reject background starts");
    options.trustedGesture = true;
    const auto started = broker.Start("widget", "media.play", options);
    Check(started && broker.ActiveCount() == 1,
        "an authorized trusted gesture must start one task");
    const auto actions = broker.DrainActions();
    Check(actions.size() == 1 &&
            actions[0].type == TaskBrokerActionType::Start &&
            actions[0].id == started.id && !actions[0].preview,
        "task start actions must preserve identity and preview isolation");
    Check(!broker.Start("widget", "media.play", options),
        "descriptor concurrency must reject duplicate active actions");
    Check(broker.Complete(started.id, true) &&
            broker.ActiveCount() == 0,
        "executors must be able to complete active tasks");
    const auto completions = broker.DrainCompletions();
    Check(completions.size() == 1 && completions[0].ok &&
            completions[0].instanceId == "widget" &&
            completions[0].name == "media.play",
        "successful completions must retain task ownership metadata");
}

void TestCancellationAndRevocation()
{
    WidgetTaskBroker broker;
    std::string error;
    Check(broker.RegisterTask(
            { "network.request", "network.internet", false, 4 }, error),
        "network task descriptor must register");
    TaskStartOptions options;
    options.permissionGranted = true;
    options.preview = true;
    const auto first = broker.Start("widget", "network.request", options);
    const auto second = broker.Start("widget", "network.request", options);
    broker.DrainActions();
    Check(first && second && broker.SetPermission(
            "widget", "network.internet", false) == 2,
        "permission revocation must cancel every matching active task");
    const auto cancelActions = broker.DrainActions();
    Check(cancelActions.size() == 2 &&
            cancelActions[0].type == TaskBrokerActionType::Cancel &&
            cancelActions[0].cancelReason ==
                TaskBrokerCancelReason::PermissionRevoked &&
            cancelActions[0].preview,
        "revocation actions must retain reason and preview state");
    Check(!broker.Cancel(first.id) &&
            broker.Complete(first.id, true) &&
            broker.Complete(second.id, false, "executorFailure"),
        "cancel requests must be idempotent and executors must acknowledge them");
    const auto completions = broker.DrainCompletions();
    Check(completions.size() == 2 &&
            !completions[0].ok &&
            completions[0].error == "permissionRevoked" &&
            !completions[1].ok &&
            completions[1].error == "permissionRevoked",
        "revocation must override late executor success or failure");
}

void TestInstanceAndShutdownCleanup()
{
    WidgetTaskBroker broker;
    std::string error;
    broker.RegisterTask({ "search", {}, false, 4 }, error);
    TaskStartOptions options;
    const auto first = broker.Start("widget-a", "search", options);
    const auto second = broker.Start("widget-b", "search", options);
    broker.DrainActions();
    Check(broker.CancelInstance("widget-a") == 1,
        "instance disposal must cancel only owned tasks");
    broker.Shutdown();
    const auto actions = broker.DrainActions();
    Check(actions.size() == 2 &&
            actions[0].id == first.id &&
            actions[0].cancelReason ==
                TaskBrokerCancelReason::InstanceDisposed &&
            actions[1].id == second.id &&
            actions[1].cancelReason == TaskBrokerCancelReason::Shutdown,
        "instance disposal and shutdown must preserve distinct cancel reasons");
    Check(broker.Complete(first.id, false) &&
            broker.Complete(second.id, false) &&
            broker.ActiveCount() == 0,
        "executor acknowledgements must release all canceled task records");
}
}

int main()
{
    TestRegistrationAndStartGuards();
    TestCancellationAndRevocation();
    TestInstanceAndShutdownCleanup();
    std::cout << "widget task broker tests passed\n";
    return 0;
}
