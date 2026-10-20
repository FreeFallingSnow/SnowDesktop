#include "widget/tasks/widget_notification_schedule_store.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace
{
using snowdesktop::widget_runtime::WidgetNotificationScheduleStore;
using snowdesktop::widget_runtime::WidgetPersistedNotificationSchedule;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestRoundTripAndOrdering()
{
    WidgetNotificationScheduleStore store;
    std::string error;
    Check(store.Upsert({ "widget-b", "package-b", "notification:2",
            "Later", "line one\nline two", 3000 }, error) &&
            store.Upsert({ "widget-a", "package-a", "notification:1",
                "\xe6\x97\xa9\xe4\xb8\x8a\xe5\xa5\xbd", "quoted \"text\"", 2000 }, error),
        "valid persisted schedules must be accepted");
    const std::string serialized = store.Serialize();
    Check(serialized.find("notification:1") <
            serialized.find("notification:2"),
        "serialized schedules must use stable due-time ordering");

    WidgetNotificationScheduleStore restored;
    Check(restored.LoadText(serialized, error) &&
            restored.Entries().size() == 2,
        "serialized schedules must round trip through the strict schema");
    const auto first = restored.Find("widget-a", "notification:1");
    Check(first && first->packageId == "package-a" &&
            first->message == "quoted \"text\"" &&
            first->dueMs == 2000,
        "round trip must preserve owner binding, UTF-8 text, and deadline");

    WidgetPersistedNotificationSchedule structured{
        "widget-c", "package-c", "notification:3",
        "Download", "In progress", 4000 };
    structured.imageResource = "preview";
    structured.progress = 0.625;
    structured.actions = { { "open", "Open" }, { "cancel", "Cancel" } };
    Check(restored.Upsert(std::move(structured), error),
        "structured schedule fixture must be accepted");
    WidgetNotificationScheduleStore structuredRestored;
    Check(structuredRestored.LoadText(restored.Serialize(), error),
        "schema v2 structured schedules must round trip");
    const auto saved = structuredRestored.Find(
        "widget-c", "notification:3");
    Check(saved && saved->imageResource == "preview" &&
            saved->progress == 0.625 && saved->actions.size() == 2 &&
            saved->actions[1].id == "cancel",
        "structured schedule round trip must preserve presentation fields");
}

void TestOwnershipMutationAndDueSelection()
{
    WidgetNotificationScheduleStore store;
    std::string error;
    Check(store.Upsert({ "widget", "package", "notification:1",
            "Title", "Message", 1000 }, error),
        "schedule fixture must be accepted");
    Check(!store.Remove("foreign", "notification:1") &&
            !store.UpdateContent("foreign", "notification:1",
                std::string("No"), std::nullopt, std::nullopt,
                std::nullopt, std::nullopt),
        "foreign instances must not mutate another schedule");
    Check(store.UpdateContent("widget", "notification:1",
            std::nullopt, std::string("Updated"), std::nullopt,
            std::nullopt, std::nullopt),
        "the owning instance must update persisted text");
    Check(store.Due(999).empty() && store.Due(1000).size() == 1,
        "due selection must include the exact epoch-millisecond deadline");

    Check(!store.Upsert({ "foreign", "other", "notification:1",
            "Title", "Message", 2000 }, error) &&
            error.find("another owner") != std::string::npos,
        "opaque IDs must not be rebound to a different instance or package");
    Check(store.RemoveInstance("widget") == 1 &&
            store.Entries().empty(),
        "instance removal must revoke every persisted schedule it owns");
}

void TestStrictSchemaRejection()
{
    WidgetNotificationScheduleStore store;
    std::string error;
    Check(!store.LoadText("{}", error) && !error.empty(),
        "missing schedule schema fields must be rejected");
    Check(!store.LoadText(
            "{\"schemaVersion\":1,\"entries\":["
            "{\"instanceId\":\"w\",\"packageId\":\"p\","
            "\"notificationId\":\"n\",\"title\":\"t\","
            "\"message\":\"m\",\"dueMs\":1.5}]} ", error),
        "fractional persisted deadlines must be rejected");
    Check(!store.LoadText(
            "{\"schemaVersion\":1,\"entries\":["
            "{\"instanceId\":\"w\",\"packageId\":\"p\","
            "\"notificationId\":\"n\",\"title\":\"t\","
            "\"message\":\"m\",\"dueMs\":1},"
            "{\"instanceId\":\"w\",\"packageId\":\"p\","
            "\"notificationId\":\"n\",\"title\":\"t\","
            "\"message\":\"m\",\"dueMs\":2}]} ", error),
        "duplicate opaque IDs must be rejected even within one owner");
    Check(!store.LoadText(
            "{\"schemaVersion\":2,\"entries\":["
            "{\"instanceId\":\"w\",\"packageId\":\"p\","
            "\"notificationId\":\"n\",\"title\":\"t\","
            "\"message\":\"m\",\"dueMs\":1,"
            "\"imageResource\":\"\",\"progress\":2,"
            "\"actions\":[]}]} ", error),
        "out-of-range structured progress must be rejected");
}

void TestCompleteEscapedCapacityAndFailedLoadGate()
{
    WidgetNotificationScheduleStore store;
    std::string error;
    for (unsigned index = 0; index < WidgetNotificationScheduleStore::MaximumEntries; ++index)
    {
        WidgetPersistedNotificationSchedule entry{
            "owner-" + std::to_string(index / 32), "package",
            "notification:" + std::to_string(index), std::string(256, '\x01'),
            std::string(2048, '\\'), 2000000000000LL + index};
        entry.imageResource = std::string(64, '\x02');
        entry.actions = {{std::string(64, '\x03'), std::string(64, '\x04')},
            {std::string(64, '\x05'), std::string(64, '\x06')}};
        Check(store.Upsert(std::move(entry), error),
            "all 16 owners may persist their full legal 32-record escaped notification quota");
    }
    auto serialized = store.Serialize();
    Check(serialized.size() > 2 * 1024 * 1024,
        "the legal full notification fixture exceeds the old reader budget");
    WidgetNotificationScheduleStore restored;
    Check(restored.LoadText(serialized, error) && restored.IsWritable() &&
            restored.Entries().size() == WidgetNotificationScheduleStore::MaximumEntries,
        "every accepted full escaped notification store survives complete reload");
    for (unsigned index = 0; index < WidgetNotificationScheduleStore::MaximumEntries; ++index)
    {
        const auto entry = restored.Find("owner-" + std::to_string(index / 32),
            "notification:" + std::to_string(index));
        Check(entry && entry->message == std::string(2048, '\\') && entry->actions.size() == 2 &&
                entry->title == std::string(256, '\x01') && entry->actions[1].label == std::string(64, '\x06'),
            "reload retains each owner's full escaped message and structured presentation");
    }
    Check(restored.UpdateContent("owner-0", "notification:0", std::nullopt,
            std::string(2048, '\x01'), std::nullopt, std::nullopt, std::nullopt),
        "updating a full store with worst-case six-byte JSON escaping remains persistable");
    WidgetNotificationScheduleStore updated;
    Check(updated.LoadText(restored.Serialize(), error),
        "the actual output of a maximum escaped-content update is readable");
    const auto beforeFailure = restored.Serialize();
    Check(!restored.LoadText("{broken", error) && !restored.IsWritable() &&
            restored.Serialize() == beforeFailure,
        "failed load preserves existing entries and closes the persistence gate");
    Check(!restored.Upsert({"owner-0", "package", "notification:0", "new", "new", 2000}, error) &&
            !restored.UpdateContent("owner-0", "notification:0", std::string("new"),
                std::nullopt, std::nullopt, std::nullopt, std::nullopt) &&
            !restored.Remove("owner-0", "notification:0") && restored.RemoveInstance("owner-0") == 0 &&
            restored.Serialize() == beforeFailure,
        "subsequent notification changes cannot turn a failed load into an empty or replacement store");
    Check(restored.LoadText(beforeFailure, error) && restored.IsWritable(),
        "only a successful complete reload reopens schedule persistence");
    restored.MarkLoadFailed();
    Check(!restored.IsWritable() && !restored.Remove("owner-0", "notification:0"),
        "an IO failure before JSON decoding closes the same persistence gate");
}
}

int main()
{
    TestRoundTripAndOrdering();
    TestOwnershipMutationAndDueSelection();
    TestStrictSchemaRejection();
    TestCompleteEscapedCapacityAndFailedLoadGate();
    std::cout << "widget notification schedule store tests passed\n";
    return 0;
}
