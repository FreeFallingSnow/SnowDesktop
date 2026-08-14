#include "widget_data_broker.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using namespace std::chrono_literals;
using snowdesktop::widget_runtime::DataBrokerActionType;
using snowdesktop::widget_runtime::DataBrokerReason;
using snowdesktop::widget_runtime::DataHiddenPolicy;
using snowdesktop::widget_runtime::DataProviderDescriptor;
using snowdesktop::widget_runtime::DataProviderState;
using snowdesktop::widget_runtime::DataSubscriptionOptions;
using snowdesktop::widget_runtime::WidgetDataBroker;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

DataProviderDescriptor CpuProvider()
{
    return {
        "system.cpu",
        "system.performance.read",
        500ms,
        5000ms,
        2000ms,
        false,
        false,
    };
}

DataProviderDescriptor AudioProvider()
{
    return {
        "audio.output.analysis",
        "audio.output.analyze",
        16ms,
        1000ms,
        5000ms,
        true,
        true,
    };
}

void TestRegistrationAndSharedSampling()
{
    WidgetDataBroker broker;
    std::string error;
    Check(!broker.RegisterProvider({}, error) && !error.empty(),
        "providers without a topic must be rejected");
    Check(broker.RegisterProvider(CpuProvider(), error),
        "a valid provider must register");
    Check(!broker.RegisterProvider(CpuProvider(), error),
        "duplicate provider topics must be rejected");
    const auto permission = broker.RequiredPermission("system.cpu");
    Check(permission && *permission == "system.performance.read" &&
            !broker.RequiredPermission("system.unknown"),
        "registered topics must expose their required permission");

    const WidgetDataBroker::TimePoint start{};
    DataSubscriptionOptions slow;
    slow.requestedInterval = 1000ms;
    slow.permissionGranted = true;
    const auto first = broker.Subscribe(
        "widget-a", "system.cpu", slow, start);
    Check(static_cast<bool>(first),
        "the first eligible subscription must be accepted");
    auto actions = broker.DrainActions();
    Check(actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Start &&
            actions[0].effectiveInterval == 1000ms,
        "the first eligible subscription must start its provider on demand");
    Check(broker.MarkStarted("system.cpu", true),
        "a starting provider must accept its start result");

    DataSubscriptionOptions fast = slow;
    fast.requestedInterval = 100ms;
    const auto second = broker.Subscribe(
        "widget-b", "system.cpu", fast, start + 10ms);
    actions = broker.DrainActions();
    Check(static_cast<bool>(second) && actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Reconfigure &&
            actions[0].effectiveInterval == 500ms,
        "shared subscribers must select one provider rate clamped to its minimum");
    const auto snapshot = broker.Snapshot("system.cpu");
    Check(snapshot && snapshot->state == DataProviderState::Active &&
            snapshot->subscriptionCount == 2 &&
            snapshot->eligibleCount == 2 && snapshot->shared,
        "provider diagnostics must report shared active subscriptions");
    const auto binding = broker.SubscriptionSnapshot(second.id);
    Check(binding && binding->instanceId == "widget-b" &&
            binding->topic == "system.cpu" && binding->eligible &&
            binding->options.requestedInterval == 500ms,
        "subscription diagnostics must expose normalized instance options");
}

void TestVisibilityAndIdleGrace()
{
    WidgetDataBroker broker;
    std::string error;
    Check(broker.RegisterProvider(CpuProvider(), error),
        "CPU provider must register");
    const WidgetDataBroker::TimePoint start{};
    DataSubscriptionOptions options;
    options.requestedInterval = 1000ms;
    options.permissionGranted = true;
    options.whenHidden = DataHiddenPolicy::Throttle;
    const auto subscription = broker.Subscribe(
        "widget", "system.cpu", options, start);
    broker.DrainActions();
    Check(broker.MarkStarted("system.cpu", true),
        "CPU provider must become active");

    Check(broker.SetInstanceVisible(
            "widget", false, start + 100ms) == 1,
        "visibility changes must reach subscribed topics");
    auto actions = broker.DrainActions();
    Check(actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Reconfigure &&
            actions[0].effectiveInterval == 5000ms,
        "hidden throttle subscriptions must lower the shared sampling rate");

    Check(broker.Unsubscribe(subscription.id, start + 200ms),
        "ordinary subscriptions must be removable");
    Check(broker.DrainActions().empty() &&
            broker.Snapshot("system.cpu")->state ==
                DataProviderState::IdleGrace,
        "ordinary providers must enter idle grace after their last subscriber");
    broker.Tick(start + 2100ms);
    Check(broker.DrainActions().empty(),
        "providers must remain alive before idle grace expires");
    broker.Tick(start + 2201ms);
    actions = broker.DrainActions();
    Check(actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Stop &&
            actions[0].reason == DataBrokerReason::IdleGraceExpired,
        "idle grace expiry must stop an unused provider");
}

void TestGraceReuseAndPermissionRevocation()
{
    WidgetDataBroker broker;
    std::string error;
    Check(broker.RegisterProvider(CpuProvider(), error),
        "CPU provider must register");
    const WidgetDataBroker::TimePoint start{};
    DataSubscriptionOptions options;
    options.permissionGranted = true;
    const auto first = broker.Subscribe(
        "widget", "system.cpu", options, start);
    broker.DrainActions();
    broker.MarkStarted("system.cpu", true);
    broker.Unsubscribe(first.id, start + 100ms);
    Check(broker.DrainActions().empty(),
        "ordinary unsubscribe must retain the provider during grace");

    const auto replacement = broker.Subscribe(
        "widget", "system.cpu", options, start + 500ms);
    auto actions = broker.DrainActions();
    Check(static_cast<bool>(replacement) && actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Reconfigure &&
            broker.Snapshot("system.cpu")->state ==
                DataProviderState::Active,
        "a new subscriber during grace must reuse the active provider");

    Check(broker.SetPermission("widget", "system.performance.read",
            false, start + 600ms) == 1,
        "permission revocation must update matching subscriptions");
    actions = broker.DrainActions();
    Check(actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Stop &&
            actions[0].reason == DataBrokerReason::PermissionRevoked &&
            broker.Snapshot("system.cpu")->permissionDeniedCount == 1,
        "permission revocation must bypass idle grace and stop immediately");
}

void TestHighRiskAndPreviewIsolation()
{
    WidgetDataBroker broker;
    std::string error;
    Check(broker.RegisterProvider(AudioProvider(), error),
        "audio analysis provider must register");
    const WidgetDataBroker::TimePoint start{};

    DataSubscriptionOptions preview;
    preview.permissionGranted = true;
    preview.preview = true;
    const auto previewSubscription = broker.Subscribe(
        "preview", "audio.output.analysis", preview, start);
    Check(static_cast<bool>(previewSubscription) &&
            broker.DrainActions().empty() &&
            broker.Snapshot("audio.output.analysis")->previewCount == 1,
        "preview subscriptions must never start a real high-risk provider");

    DataSubscriptionOptions live;
    live.requestedInterval = 33ms;
    live.permissionGranted = true;
    live.whenHidden = DataHiddenPolicy::Continue;
    const auto subscription = broker.Subscribe(
        "live", "audio.output.analysis", live, start + 1ms);
    auto actions = broker.DrainActions();
    Check(static_cast<bool>(subscription) && actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Start,
        "a visible authorized audio subscription must start capture once");
    broker.MarkStarted("audio.output.analysis", true);

    Check(broker.SetInstanceVisible("live", false, start + 2ms) == 1,
        "audio subscription visibility must be tracked");
    actions = broker.DrainActions();
    const auto snapshot = broker.Snapshot("audio.output.analysis");
    Check(actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Stop &&
            snapshot->state == DataProviderState::Stopped &&
            snapshot->hiddenCount == 1 && snapshot->eligibleCount == 0,
        "high-risk audio capture must force pause and stop at the last visible subscriber");
}

void TestShutdown()
{
    WidgetDataBroker broker;
    std::string error;
    broker.RegisterProvider(CpuProvider(), error);
    const WidgetDataBroker::TimePoint start{};
    DataSubscriptionOptions options;
    options.permissionGranted = true;
    broker.Subscribe("widget", "system.cpu", options, start);
    broker.DrainActions();
    broker.Shutdown(start + 1ms);
    const auto actions = broker.DrainActions();
    Check(actions.size() == 1 &&
            actions[0].type == DataBrokerActionType::Stop &&
            actions[0].reason == DataBrokerReason::Shutdown &&
            broker.SubscriptionCount() == 0,
        "shutdown must stop providers and release every subscription");
}
}

int main()
{
    TestRegistrationAndSharedSampling();
    TestVisibilityAndIdleGrace();
    TestGraceReuseAndPermissionRevocation();
    TestHighRiskAndPreviewIsolation();
    TestShutdown();
    std::cout << "widget data broker tests passed\n";
    return 0;
}
