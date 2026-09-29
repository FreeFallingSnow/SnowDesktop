#include "calendar_service.h"
#include "calendar_display.h"
#include "l10n.h"
#include "system_calendar_editor_state.h"

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
void CheckNativeCalendarEditor(const std::filesystem::path& root)
{
    using snowdesktop::SystemCalendarEditorState;
    using snowdesktop::ParseCalendarEditorTime;
    Expect(ParseCalendarEditorTime(L"00:00")==0&&ParseCalendarEditorTime(L"23:59")==1439&&
        !ParseCalendarEditorTime(L"24:00")&&!ParseCalendarEditorTime(L"09:60")&&!ParseCalendarEditorTime(L"9:05"),
        "native editor parses HH:mm without accepting impossible or partial times");
    CalendarService service(root/L"native-editor"/L"calendar.json");Expect(service.Load(),"native editor test store loads");
    int writes=0;bool authorized=true;
    SystemCalendarEditorState editor;
    editor.valid=[&]{return authorized;};
    editor.actions.save=[&](const CalendarEvent& event){++writes;return event.id.empty()?service.Create(event):service.Update(event.id,event.revision,event);};
    editor.actions.current=[&](const CalendarEvent& event){return service.EventById(event.id);};
    editor.actions.remove=[&](const std::string& id){++writes;return service.Remove(id);};
    CalendarEvent draft;draft.title="Popup draft";draft.date="2026-09-27";draft.startMinutes=540;draft.endMinutes=600;draft.notes="Keep this text";
    Expect(editor.Save(draft)&&editor.draft.revision==1&&service.EventById(editor.draft.id).has_value(),
        "native editor creates through the shared calendar service");
    editor.original=editor.draft;draft=editor.draft;draft.title="Changed directly";draft.date="2026-09-28";
    Expect(editor.Save(draft)&&editor.draft.revision==2&&service.EventById(editor.draft.id)->date=="2026-09-28",
        "native editor edits and reschedules the original revision");
    editor.original=editor.draft;draft=editor.draft;
    auto external=draft;external.title="External edit";Expect(service.Update(external.id,external.revision,external).ok,"external calendar change succeeds");
    draft.title="Unsaved title";draft.notes="Do not discard after conflict";
    Expect(!editor.Save(draft)&&editor.error=="conflict"&&editor.draft.title==draft.title&&editor.draft.notes==draft.notes&&editor.original.revision==2,
        "conflict preserves the user's entire draft and original revision");
    const int beforeDelete=writes;
    Expect(!editor.Remove()&&editor.error=="conflict"&&writes==beforeDelete&&service.EventById(external.id).has_value(),
        "delete confirmation cannot delete a concurrently changed event");
    editor.original=*service.EventById(external.id);
    authorized=false;
    Expect(!editor.Save(draft)&&!editor.Remove()&&writes==beforeDelete,
        "a closed popup cannot commit either a save or a confirmed deletion");
    authorized=true;
    const auto realSave=editor.actions.save;
    editor.actions.save=[](const CalendarEvent&){return snowdesktop::calendar::MutationResult{false,{},0,"write_failed"};};
    Expect(!editor.Save(draft)&&editor.draft.title==draft.title&&editor.draft.notes==draft.notes&&service.EventById(external.id)->title=="External edit",
        "persistence failure keeps the draft and stored event intact");
    editor.actions.save=realSave;
    editor.actions.current=[&](const CalendarEvent& event){authorized=false;return service.EventById(event.id);};
    Expect(!editor.Remove()&&writes==beforeDelete,"revocation during delete lookup is rechecked before mutation");
    authorized=true;editor.actions.current=[&](const CalendarEvent& event){return service.EventById(event.id);};
    Expect(editor.Remove()&&editor.deleted&&!service.EventById(external.id),"confirmed current revision can be deleted through the shared backend");
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 2)
    {
        std::cerr << "usage: SnowDesktopCalendarServiceTests <project-root>\n";
        return 2;
    }
    auto& locale = Locale::Instance();
    locale.Init((std::filesystem::path(argv[1]) / L"lang").c_str());
    Expect(!locale.GetAvailableLanguages().empty(), "calendar label tests load the real language catalogs");
    const auto rocLabel = [](const std::string& language) {
        for (const auto& option : snowdesktop::calendar::CalendarOptions(language))
            if (option.id == "roc") return option.label;
        return std::wstring{};
    };
    locale.SetLanguage("en-US");
    Expect(rocLabel("zh-CN") == L"民国纪年（公历）",
        "ROC calendar name describes its era and Gregorian dates in simplified Chinese");
    snowdesktop::calendar::DisplayPreferences rocDisplay;
    rocDisplay.enabled=true;rocDisplay.calendar="roc";
    const auto rocChinese=snowdesktop::calendar::Annotate("2026-09-29","2026-09-29",rocDisplay,"zh-CN");
    Expect(rocChinese.size()==1&&rocChinese[0].calendarAvailable&&
        rocChinese[0].fullDate.find("民国115")!=std::string::npos&&
        rocChinese[0].monthHeading.find("民国115")!=std::string::npos&&
        rocChinese[0].secondary.find("Taiwan")==std::string::npos&&
        rocChinese[0].fullDate.find("Taiwan")==std::string::npos,
        "ROC date text uses the translated era rather than ICU's Taiwan label");
    for (const auto& language : locale.GetAvailableLanguages())
    {
        locale.SetLanguage(language.code.c_str());
        const std::wstring expected = locale.TrW("settings.calendar.rocName");
        locale.SetLanguage(language.code == "en-US" ? "zh-CN" : "en-US");
        const std::string active = locale.GetLanguage();
        Expect(rocLabel(language.code) == expected && expected != L"settings.calendar.rocName",
            "calendar options use the requested language rather than the active UI language");
        Expect(locale.GetLanguage() == active, "calendar option lookup must not switch the active UI catalog");
    }
    const auto root =
        std::filesystem::temp_directory_path() /
        (L"SnowDesktopCalendarServiceTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    CheckNativeCalendarEditor(root);

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

    // Independent civil dates protect real conversion, holiday selection and off-state semantics.
    using snowdesktop::calendar::Annotate;
    snowdesktop::calendar::DisplayPreferences display;
    display.enabled = true;
    auto lunar = Annotate("2024-02-10", "2024-02-10", display, "zh-CN");
    Expect(lunar.size() == 1 && lunar[0].calendarAvailable && lunar[0].month == 1 && lunar[0].day == 1 && !lunar[0].leapMonth,
        "Chinese New Year maps to month one day one");
    lunar = Annotate("2023-03-22", "2023-03-22", display, "zh-CN");
    Expect(lunar.size() == 1 && lunar[0].month == 2 && lunar[0].day == 1 && lunar[0].leapMonth, "Chinese leap month is preserved");
    Expect(lunar[0].secondary == "闰二月", "first lunar day displays leap month name");
    Expect(Annotate("2024-02-11", "2024-02-11", display, "zh-CN")[0].secondary == "初二", "ordinary lunar date uses traditional day name");
    Expect(Annotate("2024-03-01", "2024-03-01", display, "zh-CN")[0].secondary == "廿一", "lunar day twenty one is compact");
    Expect(Annotate("2026-10-01", "2026-10-01", display, "zh-CN")[0].fullDate.find(" 星期四") != std::string::npos,
        "full lunar date separates the weekday with a space");
    Expect(Annotate("2024-02-10", "2024-02-10", display, "zh-CN")[0].secondary == "正月", "lunar new year caption is month only");
    const auto cyclical=Annotate("2026-09-29","2026-09-29",display,"zh-CN");
    Expect(cyclical.size()==1&&cyclical[0].monthHeading.starts_with("丙午年")&&
        cyclical[0].fullDate.starts_with("丙午年")&&
        cyclical[0].fullDate.find("2026")==std::string::npos,
        "Chinese lunar dates use the sexagenary year without a Gregorian year prefix");
    display.holidaysEnabled = true; display.region = "CN";
    const auto formerHoliday = Annotate("2024-10-01", "2024-10-01", display, "zh-CN");
    Expect(formerHoliday.size() == 1 && formerHoliday[0].calendarAvailable && formerHoliday[0].holidays.empty() && !formerHoliday[0].holidaysAvailable,
        "legacy holiday preferences cannot restore removed holiday display");
    display.calendar = "persian";
    auto persian = Annotate("2024-03-20", "2024-03-20", display, "en-US");
    Expect(persian.size() == 1 && persian[0].year == 1403 && persian[0].month == 1 && persian[0].day == 1, "Persian New Year conversion");
    display.enabled = false;
    auto disabled = Annotate("2024-10-01", "2024-10-01", display, "zh-CN");
    Expect(disabled.size() == 1 && !disabled[0].calendarAvailable && disabled[0].secondary.empty() && disabled[0].holidays.empty(), "disabled extra calendar leaves no annotations");
    Expect(Annotate("2024-02-30", "2024-03-01", display, "en-US").empty() &&
        Annotate("2024-03-01", "2024-02-01", display, "en-US").empty() &&
        Annotate("2024-01-01", "2024-04-01", display, "en-US").empty(), "annotation input and work limits are enforced");
    for (const auto& option : snowdesktop::calendar::CalendarOptions("en-US"))
    {
        display.enabled = true; display.calendar = option.id;
        const auto sample = Annotate("2024-02-10", "2024-02-10", display, "en-US");
        Expect(!option.label.empty() && sample.size() == 1 && sample[0].calendarAvailable, "advertised calendar converts with installed ICU");
    }

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

    CalendarEvent linked;
    linked.title="Countdown"; linked.date="2026-09-11"; linked.allDay=true;
    linked.reminderMinutes=-1;
    const auto createdLink=service.Create(linked);
    Expect(createdLink.ok,"linked event created through production service");
    auto found=service.EventById(createdLink.id);
    Expect(found && found->date=="2026-09-11","ID lookup finds the event");
    linked.title="Renamed";linked.date="2035-02-28";
    const auto moved=service.Update(createdLink.id,createdLink.revision,linked);
    found=service.EventById(createdLink.id);
    Expect(moved.ok && found && found->title=="Renamed" && found->date=="2035-02-28",
        "stable ID follows rename and rescheduling outside the old date window");
    Expect(service.Remove(createdLink.id).ok && !service.EventById(createdLink.id),
        "deleted IDs return no event");
    Expect(!service.EventById("missing"),"unknown ID does not select another event");

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
