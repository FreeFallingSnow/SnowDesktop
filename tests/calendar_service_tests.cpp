#include "calendar_service.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
using snowdesktop::calendar::CalendarEvent;
using snowdesktop::calendar::CalendarNow;
using snowdesktop::calendar::CalendarService;

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

void Write(
    const std::filesystem::path& path,
    const std::string& text)
{
    std::filesystem::create_directories(
        path.parent_path());
    std::ofstream file(
        path,
        std::ios::binary | std::ios::trunc);
    file << text;
}
}

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        (L"SnowDesktopCalendarServiceTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    const auto leap =
        CalendarService::GetDateInfo("2024-02-29");
    Expect(
        leap && leap->weekday == 5 &&
            leap->daysInMonth == 29,
        "leap day and weekday are calculated");
    Expect(
        !CalendarService::GetDateInfo("2023-02-29"),
        "invalid leap day is rejected");
    Expect(
        CalendarService::AddDays(
            "2024-02-28", 2) ==
            std::optional<std::string>("2024-03-01"),
        "date shifting crosses leap month");
    Expect(
        CalendarService::AddDays(
            "2025-12-31", 1) ==
            std::optional<std::string>("2026-01-01"),
        "date shifting crosses year");

    CalendarNow now{ "2026-07-30", 9 * 60 + 40 };
    const auto path = root / L"SnowDesktop.calendar.json";
    CalendarService service(
        path, [&] { return now; });
    int eventChanges = 0;
    int selectionChanges = 0;
    service.SetChangedCallback(
        [&](const std::string& reason) {
            if (reason == "events")
                ++eventChanges;
            if (reason == "selection")
                ++selectionChanges;
        });
    Expect(service.Load(), "empty calendar loads");
    Expect(
        service.SelectedDate() == "2026-07-30",
        "selected date defaults to today");
    Expect(
        service.SetSelectedDate("2026-08-01") &&
            selectionChanges == 1,
        "valid selected date is shared");
    Expect(
        !service.SetSelectedDate("2026-02-30"),
        "invalid selected date is rejected");

    CalendarNow trackingNow{ "2026-07-30", 8 * 60 };
    CalendarService trackingService(
        root / L"tracking" / L"calendar.json",
        [&] { return trackingNow; });
    int trackingChanges = 0;
    trackingService.SetChangedCallback(
        [&](const std::string& reason) {
            if (reason == "selection")
                ++trackingChanges;
        });
    Expect(
        trackingService.Load() &&
            trackingService.SelectedDate() ==
                "2026-07-30",
        "today tracking starts with the local date");
    trackingNow.date = "2026-07-31";
    trackingService.Tick();
    Expect(
        trackingService.SelectedDate() ==
                "2026-07-31" &&
            trackingChanges == 1,
        "default selection follows midnight");
    Expect(
        trackingService.SetSelectedDate("2026-08-05"),
        "a custom date can be selected");
    trackingNow.date = "2026-08-01";
    trackingService.Tick();
    Expect(
        trackingService.SelectedDate() ==
            "2026-08-05",
        "custom selection is preserved across midnight");

    CalendarEvent later;
    later.title = "Later";
    later.date = "2026-07-31";
    later.startMinutes = 10 * 60;
    later.endMinutes = 11 * 60;
    later.reminderMinutes = -1;
    const auto laterCreated = service.Create(later);
    Expect(
        laterCreated.ok && laterCreated.revision == 1,
        "timed event is created");

    CalendarEvent allDay;
    allDay.title = "All day";
    allDay.date = "2026-07-30";
    allDay.allDay = true;
    allDay.reminderMinutes = -1;
    const auto allDayCreated = service.Create(allDay);
    Expect(
        allDayCreated.ok,
        "all-day event is created");

    CalendarEvent timed;
    timed.title = "Timed";
    timed.date = "2026-07-30";
    timed.startMinutes = 10 * 60;
    timed.endMinutes = 11 * 60;
    timed.notes = "note";
    timed.reminderMinutes = 15;
    const auto timedCreated = service.Create(timed);
    Expect(
        timedCreated.ok && eventChanges == 3,
        "event changes are broadcast");

    CalendarEvent invalid = timed;
    invalid.title = "Invalid";
    invalid.startMinutes = 12 * 60;
    invalid.endMinutes = 11 * 60;
    Expect(
        !service.Create(invalid).ok,
        "backwards time range is rejected");

    const auto julyEvents = service.Events(
        "2026-07-30", "2026-07-31");
    Expect(
        julyEvents.size() == 3 &&
            julyEvents[0].id == allDayCreated.id &&
            julyEvents[1].id == timedCreated.id &&
            julyEvents[2].id == laterCreated.id,
        "events sort by date, all-day, and start time");

    CalendarEvent edited = timed;
    edited.title = "Edited";
    Expect(
        !service.Update(
             timedCreated.id, 99, edited).ok,
        "stale revision is rejected");
    const auto updated = service.Update(
        timedCreated.id, timedCreated.revision, edited);
    Expect(
        updated.ok && updated.revision == 2,
        "matching revision updates event");

    int notifications = 0;
    std::string notifiedTitle;
    service.SetNotificationCallback(
        [&](const CalendarEvent& event) {
            ++notifications;
            notifiedTitle = event.title;
        });
    now.minutes = 10 * 60 - 14;
    service.CheckReminders(now, true);
    Expect(
        notifications == 1 &&
            notifiedTitle == "Edited",
        "startup catches a due reminder today");
    service.CheckReminders(now, false);
    Expect(
        notifications == 1,
        "reminder is persisted and deduplicated");

    CalendarEvent titleOnly = edited;
    titleOnly.title = "Renamed";
    titleOnly.notes = "changed note";
    const auto titleUpdated = service.Update(
        timedCreated.id, updated.revision, titleOnly);
    service.CheckReminders(now, true);
    Expect(
        titleUpdated.ok && notifications == 1,
        "title and notes changes do not repeat a reminder");

    CalendarEvent rescheduled = titleOnly;
    rescheduled.startMinutes = 10 * 60 + 30;
    rescheduled.endMinutes = 11 * 60 + 30;
    const auto scheduleUpdated = service.Update(
        timedCreated.id, titleUpdated.revision, rescheduled);
    now.minutes = 10 * 60 + 16;
    service.CheckReminders(now, true);
    Expect(
        scheduleUpdated.ok && notifications == 2 &&
            notifiedTitle == "Renamed",
        "schedule changes reset reminder delivery state");

    CalendarService reloaded(
        path, [&] { return now; });
    int reloadedNotifications = 0;
    reloaded.SetNotificationCallback(
        [&](const CalendarEvent&) {
            ++reloadedNotifications;
        });
    Expect(
        reloaded.Load() &&
            reloaded.Events(
                "2026-07-30",
                "2026-07-31").size() == 3,
        "events persist and reload");
    reloaded.CheckReminders(now, true);
    Expect(
        reloadedNotifications == 0,
        "notification dedupe survives restart");

    const auto removed =
        reloaded.Remove(laterCreated.id);
    Expect(
        removed.ok &&
            reloaded.Events(
                "2026-07-31",
                "2026-07-31").empty(),
        "event deletion persists");

    const auto corruptPath =
        root / L"corrupt" / L"SnowDesktop.calendar.json";
    Write(corruptPath, "{not-json");
    CalendarService corrupt(
        corruptPath, [&] { return now; });
    Expect(
        !corrupt.Load() &&
            corrupt.Events(
                "2026-01-01",
                "2026-12-31").empty() &&
            !std::filesystem::exists(corruptPath),
        "malformed calendar is quarantined");
    bool quarantineFound = false;
    for (const auto& entry :
        std::filesystem::directory_iterator(
            corruptPath.parent_path()))
    {
        if (entry.path().filename().wstring().find(
                L".corrupt-") != std::wstring::npos)
            quarantineFound = true;
    }
    Expect(
        quarantineFound,
        "quarantined calendar remains recoverable");

    const auto fractionalPath =
        root / L"fractional" / L"SnowDesktop.calendar.json";
    Write(
        fractionalPath,
        "{\"schemaVersion\":1,\"events\":[{"
        "\"id\":\"fractional\",\"revision\":1.5,"
        "\"title\":\"Invalid\",\"date\":\"2026-07-30\","
        "\"allDay\":false,\"startMinutes\":600,"
        "\"endMinutes\":660,\"notes\":\"\","
        "\"reminderMinutes\":15,"
        "\"notifiedTrigger\":\"\"}]}");
    CalendarService fractional(
        fractionalPath, [&] { return now; });
    Expect(
        !fractional.Load() &&
            !std::filesystem::exists(fractionalPath),
        "fractional persisted integer fields are quarantined");

    std::filesystem::remove_all(root, error);
    if (failures == 0)
        std::cout << "calendar service tests passed\n";
    return failures == 0 ? 0 : 1;
}
