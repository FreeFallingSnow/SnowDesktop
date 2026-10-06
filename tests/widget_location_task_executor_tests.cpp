#include "widget/tasks/widget_location_task_executor.h"
#include "widget/tasks/widget_task_broker.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace snowdesktop::widget_runtime;
static void Check(bool value, const char* message)
{ if (!value) { std::cerr << message << '\n'; std::exit(1); } }
int main()
{
    LocationOptions options;
    Check(ParseLocationOptions({}, options) && options.timeoutMs == 10000 && options.maximumAgeMs == 300000, "location defaults");
    Check(ParseLocationOptions({{"timeoutMs", "1000"}, {"maximumAgeMs", "0"}}, options), "location boundary options");
    Check(!ParseLocationOptions({{"timeoutMs", "30001"}}, options) && !ParseLocationOptions({{"maximumAgeMs", "-1"}}, options) &&
        !ParseLocationOptions({{"timeoutMs", "1000junk"}}, options) && !ParseLocationOptions({{"accuracy", "1"}}, options), "strict bounded options");
    LocationResult result; result.latitude = 38.91; result.longitude = 121.61;
    result.accuracyMeters = 50; result.timestampMs = 1780000000000;
    Check(ValidLocationResult(result), "realistic position");
    result.latitude = std::numeric_limits<double>::quiet_NaN(); Check(!ValidLocationResult(result), "nonfinite rejected");
    result.latitude = 91; Check(!ValidLocationResult(result), "range rejected");
    result.latitude = 38; result.accuracyMeters = -1; Check(!ValidLocationResult(result), "negative accuracy rejected");
    WidgetLocationTaskExecutor executor; int wakes = 0;
    executor.SetCompletionCallback([&] { ++wakes; });
    Check(!executor.Start(0, {}, true) && !executor.Start(3, {500, 0}, true), "invalid request rejected");
    Check(executor.Start(4, {}, true), "preview without sensors or consent dialog");
    const auto previews = executor.DrainCompletions();
    Check(wakes == 1 && previews.size() == 1 && previews[0].id == 4 && !previews[0].ok &&
        previews[0].error == "previewUnavailable", "preview cannot claim a position");
    Check(executor.DrainCompletions().empty() && !executor.Cancel(4), "delivered once");
    WidgetTaskBroker broker; std::string error;
    Check(broker.RegisterTask({"location.current", "location.read", true, 1}, error), "register location");
    TaskStartOptions start; start.ownerToken = 9; start.trustedGesture = true;
    Check(broker.Start("one", "location.current", start).error == "permissionDenied", "permission required");
    start.permissionGranted = true; start.trustedGesture = false;
    Check(broker.Start("one", "location.current", start).error == "userGestureRequired", "gesture required");
    start.trustedGesture = true; const auto request = broker.Start("one", "location.current", start);
    Check(static_cast<bool>(request) && !broker.Start("one", "location.current", start), "authorized start and concurrency bound");
    Check(broker.SetPermission("one", "location.read", false) == 1 && broker.Complete(request.id, true), "revocation and late completion");
    const auto done = broker.DrainCompletions();
    Check(done.size() == 1 && !done[0].ok && done[0].error == "permissionRevoked", "revoked position cannot succeed");
    const auto cancel = broker.Start("two", "location.current", start);
    Check(cancel && broker.Cancel(cancel.id) && broker.Complete(cancel.id, true), "cancel in-flight position");
    const auto canceled = broker.DrainCompletions();
    Check(canceled.size() == 1 && !canceled[0].ok && canceled[0].error == "canceled", "canceled result cannot succeed");
    const auto disposed = broker.Start("three", "location.current", start);
    Check(disposed && broker.CancelInstance("three") == 1 && broker.Complete(disposed.id, true), "dispose owner");
    const auto disposedCompletion = broker.DrainCompletions();
    Check(disposedCompletion.size() == 1 && !disposedCompletion[0].ok &&
        disposedCompletion[0].error == "instanceDisposed", "disposed position cannot succeed before engine owner filtering");
    std::cout << "location option, preview and permission checks passed\n";
}
