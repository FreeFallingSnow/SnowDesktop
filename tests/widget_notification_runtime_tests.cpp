#include "widget_notification_runtime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using snowdesktop::widget_runtime::WidgetNotificationCenter;
using snowdesktop::widget_runtime::WidgetNotificationAction;
using snowdesktop::widget_runtime::WidgetNotificationContent;
using snowdesktop::widget_runtime::WidgetNotificationHostOperation;
using snowdesktop::widget_runtime::WidgetNotificationHostRequest;
using snowdesktop::widget_runtime::WidgetNotificationPatch;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestImmediateLifecycleAndOwnership()
{
    WidgetNotificationCenter center;
    const auto now = WidgetNotificationCenter::Clock::time_point(
        std::chrono::hours(100));
    std::vector<WidgetNotificationHostRequest> requests;
    const auto host = [&requests](const WidgetNotificationHostRequest& request) {
        requests.push_back(request);
        return true;
    };
    const auto shown = center.Show(
        101, { L"Timer", L"Complete" }, now, host);
    Check(shown.ok && !shown.id.empty() && requests.size() == 1 &&
            requests.back().operation ==
                WidgetNotificationHostOperation::Show &&
            requests.back().id == shown.id,
        "show must allocate an opaque ID before presenting through the host");

    const auto foreign = center.Update(
        202, shown.id, { L"Other" }, host);
    Check(!foreign.ok && foreign.error == "notFound" &&
            requests.size() == 1,
        "notification IDs must remain scoped to their owning VM generation");

    const auto updated = center.Update(
        101, shown.id, { std::nullopt, L"Ready" }, host);
    Check(updated.ok && requests.size() == 2 &&
            requests.back().operation ==
                WidgetNotificationHostOperation::Update &&
            requests.back().title == L"Timer" &&
            requests.back().message == L"Ready",
        "update must preserve omitted fields and target the same host ID");

    const auto dismissed = center.Dismiss(101, shown.id, host);
    Check(dismissed.ok && requests.size() == 3 &&
            requests.back().operation ==
                WidgetNotificationHostOperation::Dismiss &&
            center.CountForOwner(101) == 0,
        "dismiss must remove a delivered notification after host acceptance");
    Check(!center.Dismiss(101, shown.id, host).ok,
        "dismissed IDs must not be reusable");
}

void TestSchedulingAndDelivery()
{
    WidgetNotificationCenter center;
    const auto now = WidgetNotificationCenter::Clock::time_point(
        std::chrono::hours(200));
    const auto due = now + std::chrono::minutes(5);
    int hostCalls = 0;
    const auto host = [&hostCalls](const WidgetNotificationHostRequest&) {
        ++hostCalls;
        return true;
    };
    const auto scheduled = center.Schedule(
        301, { L"Break", L"Stand up" }, due, now);
    Check(scheduled.ok && center.CountForOwner(301) == 1,
        "schedule must reserve a bounded instance-scoped notification ID");
    Check(center.DispatchDue(now + std::chrono::minutes(4),
            [](std::uint64_t) { return std::string{}; }, host).empty() &&
            hostCalls == 0,
        "scheduled notifications must not poll or deliver before their deadline");

    const auto deliveries = center.DispatchDue(due,
        [](std::uint64_t owner) {
            return owner == 301 ? std::string{} :
                std::string("instanceDisposed");
        }, host);
    Check(deliveries.size() == 1 && deliveries[0].ok &&
            deliveries[0].id == scheduled.id && hostCalls == 1,
        "a due notification must pass admission once and report delivery");
    Check(!center.Cancel(301, scheduled.id).ok &&
            center.Dismiss(301, scheduled.id, host).ok,
        "delivered schedules must transition from cancelable to dismissible");
}

void TestCancelFailureAndCleanup()
{
    WidgetNotificationCenter center;
    const auto now = WidgetNotificationCenter::Clock::time_point(
        std::chrono::hours(300));
    const auto first = center.Schedule(401, { L"One", L"First" },
        now + std::chrono::seconds(1), now);
    const auto second = center.Schedule(402, { L"Two", L"Second" },
        now + std::chrono::seconds(1), now);
    Check(first.ok && second.ok && center.Cancel(401, first.id).ok &&
            center.CountForOwner(401) == 0,
        "cancel must remove only a still-scheduled owned record");
    const auto failed = center.DispatchDue(now + std::chrono::seconds(1),
        [](std::uint64_t) { return std::string("quotaExceeded"); },
        [](const WidgetNotificationHostRequest&) { return true; });
    Check(failed.size() == 1 && !failed[0].ok &&
            failed[0].error == "quotaExceeded" &&
            center.CountForOwner(402) == 0,
        "failed due delivery must report a stable error and release its record");

    int dismissals = 0;
    const auto shown = center.Show(501, { L"Live", L"Visible" }, now,
        [&dismissals](const WidgetNotificationHostRequest& request) {
            if (request.operation ==
                WidgetNotificationHostOperation::Dismiss)
                ++dismissals;
            return true;
        });
    Check(shown.ok, "cleanup fixture notification must show");
    center.RemoveOwner(501,
        [&dismissals](const WidgetNotificationHostRequest& request) {
            if (request.operation ==
                WidgetNotificationHostOperation::Dismiss)
                ++dismissals;
            return true;
        });
    Check(dismissals == 1 && center.CountForOwner(501) == 0,
        "instance disposal must dismiss delivered records and cancel schedules");
}

void TestRestoreAcrossRuntimeGeneration()
{
    WidgetNotificationCenter center;
    const auto now = WidgetNotificationCenter::Clock::time_point(
        std::chrono::hours(400));
    const auto restored = center.RestoreScheduled(601,
        "notification:259:1", { L"Restored", L"Pending" },
        now + std::chrono::minutes(2), now);
    Check(restored.ok &&
            restored.id == "notification:259:1" &&
            center.Update(601, restored.id,
                { std::nullopt, L"Rebound" }, {}).ok,
        "a persisted ID must rebind to the new runtime owner token");
    Check(!center.Cancel(602, restored.id).ok &&
            center.Show(601, { L"New", L"Unique" }, now,
                [](const WidgetNotificationHostRequest&) {
                    return true;
                }).id != restored.id &&
            center.Cancel(601, restored.id).ok,
        "restored IDs must remain isolated from other VM generations");
    Check(!center.RestoreScheduled(601, "expired",
            { L"Old", L"Missed" },
            now - std::chrono::hours(25), now).ok,
        "startup catch-up must discard schedules missed by more than one day");
}

void TestStructuredContentAndActionActivation()
{
    WidgetNotificationCenter center;
    const auto now = WidgetNotificationCenter::Clock::time_point(
        std::chrono::hours(500));
    std::vector<WidgetNotificationHostRequest> requests;
    const auto host = [&requests](const WidgetNotificationHostRequest& request) {
        requests.push_back(request);
        return true;
    };
    WidgetNotificationContent content;
    content.title = L"Download";
    content.message = L"In progress";
    content.imagePath = L"C:\\package\\preview.png";
    content.progress = 0.25;
    content.actions = { { "open", L"Open" }, { "cancel", L"Cancel" } };
    const auto shown = center.Show(701, std::move(content), now, host);
    Check(shown.ok && requests.size() == 1 &&
            requests[0].imagePath == L"C:\\package\\preview.png" &&
            requests[0].progress == 0.25 &&
            requests[0].actions.size() == 2,
        "structured show must preserve package image, progress, and actions");

    WidgetNotificationPatch patch;
    patch.progress.emplace(std::optional<double>{});
    patch.actions = std::vector<WidgetNotificationAction>{
        { "open", L"View" } };
    Check(center.Update(701, shown.id, std::move(patch), host).ok &&
            requests.size() == 2 && !requests.back().progress &&
            requests.back().actions.size() == 1 &&
            requests.back().actions[0].label == L"View",
        "structured update must support clearing progress and replacing actions");
    Check(!center.Activate(shown.id, "missing").ok &&
            center.Activate(shown.id, "open").ok &&
            center.CountForOwner(701) == 0,
        "only declared actions may consume a delivered notification");

    WidgetNotificationContent duplicate;
    duplicate.title = L"Invalid";
    duplicate.message = L"Duplicate";
    duplicate.actions = { { "same", L"One" }, { "same", L"Two" } };
    Check(!center.Show(701, std::move(duplicate), now, host).ok,
        "duplicate action IDs must be rejected before host presentation");
}
}

int main()
{
    TestImmediateLifecycleAndOwnership();
    TestSchedulingAndDelivery();
    TestCancelFailureAndCleanup();
    TestRestoreAcrossRuntimeGeneration();
    TestStructuredContentAndActionActivation();
    std::cout << "widget notification runtime tests passed\n";
    return 0;
}
