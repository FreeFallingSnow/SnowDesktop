#include "widget_task_broker.h"
#include "widget_trusted_gesture.h"

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
using snowdesktop::widget_runtime::IsTrustedWidgetGestureCallback;
using snowdesktop::widget_runtime::WidgetTrustedGestureScope;
using snowdesktop::widget_runtime::WidgetTrustedGestureState;

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
            { "media.toggle", "media.action", true, 1 }, error) &&
            !broker.RegisterTask(
                { "media.toggle", "media.action", true, 1 }, error),
        "task descriptors must register once");
    const auto permission = broker.RequiredPermission("media.toggle");
    Check(permission && *permission == "media.action" &&
            !broker.RequiredPermission("missing"),
        "registered tasks must expose their required permission");

    TaskStartOptions options;
    const auto ownerless = broker.Start("widget", "media.toggle", options);
    Check(!ownerless && ownerless.error == "task owner token is required",
        "tasks must identify the owning Lua VM generation");
    options.ownerToken = 101;
    Check(!broker.Start("widget", "missing", options) &&
            !broker.Start("widget", "media.toggle", options),
        "unknown and unauthorized tasks must be rejected");
    options.permissionGranted = true;
    Check(!broker.Start("widget", "media.toggle", options),
        "gesture-required tasks must reject background starts");
    options.trustedGesture = true;
    options.arguments.emplace("query", "snow");
    const auto started = broker.Start("widget", "media.toggle", options);
    Check(started && broker.ActiveCount() == 1,
        "an authorized trusted gesture must start one task");
    const auto actions = broker.DrainActions();
    Check(actions.size() == 1 &&
            actions[0].type == TaskBrokerActionType::Start &&
            actions[0].id == started.id &&
            actions[0].ownerToken == 101 && !actions[0].preview &&
            actions[0].arguments.at("query") == "snow",
        "task start actions must preserve identity, arguments, and preview isolation");
    Check(!broker.Start("widget", "media.toggle", options),
        "descriptor concurrency must reject duplicate active actions");
    Check(broker.Complete(started.id, true) &&
            broker.ActiveCount() == 0,
        "executors must be able to complete active tasks");
    const auto completions = broker.DrainCompletions();
    Check(completions.size() == 1 && completions[0].ok &&
            completions[0].ownerToken == 101 &&
            completions[0].instanceId == "widget" &&
            completions[0].name == "media.toggle",
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
    options.ownerToken = 201;
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
    options.ownerToken = 301;
    const auto first = broker.Start("widget-a", "search", options);
    options.ownerToken = 302;
    const auto second = broker.Start("widget-b", "search", options);
    broker.DrainActions();
    Check(broker.CancelInstance("widget-a") == 1,
        "instance disposal must cancel only owned tasks");
    broker.Shutdown();
    const auto afterShutdown = broker.Start("widget-b", "search", options);
    Check(!afterShutdown &&
            afterShutdown.error == "task broker is shut down",
        "shutdown must reject tasks started by later dispose callbacks");
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

void TestTrustedGestureScope()
{
    Check(IsTrustedWidgetGestureCallback("onClick") &&
            IsTrustedWidgetGestureCallback("onPanelMouseDown") &&
            IsTrustedWidgetGestureCallback("onWheel") &&
            !IsTrustedWidgetGestureCallback("onMouseMove") &&
            !IsTrustedWidgetGestureCallback("onPanelOpened"),
        "only direct pointer activations may open a trusted gesture scope");
    WidgetTrustedGestureState state;
    Check(!state.Active(), "gesture state must be inactive by default");
    {
        WidgetTrustedGestureScope outer(state, true);
        Check(state.Active(),
            "trusted callbacks must activate the gesture state");
        {
            WidgetTrustedGestureScope untrustedNested(state, false);
            Check(!state.Active(),
                "untrusted nested callbacks must not inherit activation");
        }
        Check(state.Active(),
            "nested callbacks must restore the outer gesture state");
    }
    Check(!state.Active(),
        "gesture activation must not escape its synchronous callback");
}
}

int main()
{
    TestRegistrationAndStartGuards();
    TestCancellationAndRevocation();
    TestInstanceAndShutdownCleanup();
    TestTrustedGestureScope();
    std::cout << "widget task broker tests passed\n";
    return 0;
}
