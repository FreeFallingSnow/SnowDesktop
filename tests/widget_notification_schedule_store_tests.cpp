#include "widget_notification_schedule_store.h"

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
}

int main()
{
    TestRoundTripAndOrdering();
    TestOwnershipMutationAndDueSelection();
    TestStrictSchemaRejection();
    std::cout << "widget notification schedule store tests passed\n";
    return 0;
}
