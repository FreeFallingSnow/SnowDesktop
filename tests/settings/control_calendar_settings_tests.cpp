#include "settings/control_calendar_settings.h"
#include <objbase.h>
#include <filesystem>
#include <iostream>
#include <memory>

PersonalizationSettings PersonalizationSettings::DarkPreset() { return {}; }
PersonalizationSettings PersonalizationSettings::AcrylicDarkPreset() { return {}; }
CategorySettings CategorySettings::Defaults() { return {}; }

namespace
{
using namespace snowdesktop;
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
int failures = 0;
void Check(bool value, const char* message)
{ if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }
template<class Function> void Reject(std::string_view code, Function function)
{
    try { function(); Check(false, "expected rejection"); }
    catch (const Error& error) { Check(error.code == code, "stable error code"); }
}

struct Controller final : ISettingsController
{
    std::shared_ptr<SettingsSnapshot> state = std::make_shared<SettingsSnapshot>();
    Controller() { state->generation = 6; state->revision = 9; }
    SettingsActionResult Initialize() override { return {}; }
    SettingsActionResult Reload(SettingsReloadPolicy) override { return {}; }
    SettingsActionResult Open(SettingsRoute) override { return {}; }
    SettingsActionResult CloseSession() override { return {}; }
    void SetSnapshotChangedCallback(SnapshotChangedCallback) override {}
    void SetPendingWorkCallback(PendingWorkCallback) override {}
    SnapshotPtr Snapshot() const noexcept override { return state; }
    std::uint64_t Generation() const noexcept override { return state->generation; }
    bool IsGenerationCurrent(std::uint64_t generation) const noexcept override { return generation == state->generation; }
    void UpdatePersonalization(PersonalizationSettings, SettingsUpdateMode) override {}
    void UpdateDock(DockSettings, SettingsUpdateMode) override {}
    void UpdateNavigation(NavigationSettings, SettingsUpdateMode) override {}
    void UpdateGeneral(GeneralSettings, SettingsUpdateMode) override {}
    void UpdateCategory(CategorySettings, SettingsUpdateMode) override {}
    void UpdateDesktop(DesktopDisplaySettings, SettingsUpdateMode) override {}
    void RequestCommit(SettingsDomain) override {}
    SettingsActionResult FlushPending() override { return {}; }
    SettingsActionResult FlushAll() override { return {}; }
    bool RetryPending() override { return false; }
    void PrepareForExternalDataReplacement() override {}
    SettingsActionResult InvokeHostAction(const SettingsHostActions::Request&) override { return {}; }
};
struct TemporaryDirectory
{
    std::filesystem::path root, base;
    TemporaryDirectory()
    {
        base = std::filesystem::temp_directory_path();
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("GUID failed");
        wchar_t text[40]{}; StringFromGUID2(id, text, 40);
        root = base / (std::wstring(L"SnowDesktop-control-calendar-") + text);
        std::filesystem::create_directory(root);
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        if (root.parent_path().lexically_normal() == base.lexically_normal() &&
            root.filename().wstring().starts_with(L"SnowDesktop-control-calendar-"))
            std::filesystem::remove_all(root, error);
    }
};

struct Fixture
{
    TemporaryDirectory directory;
    Controller controller;
    calendar::CalendarService service{directory.root / L"calendar.json", [] { return calendar::CalendarNow{"2026-10-29", 720}; }};
    int writes = 0;
    CalendarSettingsContext context{controller, {}};
    Fixture()
    {
        Check(service.Load(), "isolated calendar load");
        context.calendar.events = [&](std::uint64_t) { return std::optional(service.SingleEvents()); };
        context.calendar.series = [&](std::uint64_t) { return std::optional(service.Series()); };
        context.calendar.occurrence = [&](std::uint64_t, std::string id) { return service.EventById(id); };
        context.calendar.mutate = [&](std::uint64_t, calendar::CalendarEvent event, bool remove) {
            ++writes;
            if (remove) return service.Remove(event.id, event.revision);
            return event.id.empty() ? service.Create(event) : service.Update(event.id, event.revision, event);
        };
        context.calendar.mutateSeries = [&](std::uint64_t, calendar::CalendarSeries series, bool remove) {
            ++writes;
            if (remove) return service.RemoveSeries(series.id, series.revision);
            return series.id.empty() ? service.CreateSeries(series) : service.UpdateSeries(series.id, series.revision, series);
        };
    }
    Value Get(const Target& target)
    { return HandleCalendarSettings("settings.get", Object({{"target", TargetJson(target)}}), target, context); }
    Value Mutate(std::string_view method, const Target& target, Value values, std::string action = {}, bool confirmed = false)
    {
        if (confirmed) values.object.emplace("confirmed", Boolean(true));
        auto params = Object({{"target", TargetJson(target)}, {"ifRevision", Get(target).object.at("revision")},
            {method == "settings.action" ? "arguments" : "changes", std::move(values)}});
        if (!action.empty()) params.object.emplace("action", String(std::move(action)));
        return HandleCalendarSettings(method, params, target, context);
    }
};

void Events()
{
    Fixture fixture;
    const Target all{"calendar", {}};
    auto fields = Object({{"title", String("  Capture review  ")}, {"date", String("2026-10-29")},
        {"startMinutes", Number(600)}, {"endMinutes", Number(630)}, {"reminderMinutes", Number(15)}});
    const auto created = fixture.Mutate("settings.action", all, fields, "create-event");
    const Target event{"calendar-event", created.object.at("id").string};
    const auto old = fixture.Get(event);
    Check(old.object.at("values").object.at("title").string == "Capture review", "real service normalizes event title");
    Check(!fixture.controller.state->sessionActive, "headless reads and callbacks do not open a UI session");
    fixture.Mutate("settings.update", event, Object({{"notes", String("Use real files")}}));
    fixture.Mutate("settings.update", event, Object({{"notes", String("")}}));
    Check(fixture.service.EventById(event.id)->notes.empty(), "empty optional notes can be cleared");
    auto stale = Object({{"target", TargetJson(event)}, {"ifRevision", old.object.at("revision")}, {"changes", Object({{"title", String("stale")}})}});
    Reject("stale_settings", [&] { HandleCalendarSettings("settings.update", stale, event, fixture.context); });
    const auto before = fixture.writes;
    Reject("invalid_params", [&] { fixture.Mutate("settings.update", event, Object({{"hostRevision", Number(999)}})); });
    Reject("invalid_params", [&] { fixture.Mutate("settings.update", event, Object({{"startMinutes", Number(1440)}})); });
    Check(fixture.writes == before, "invalid fields never reach service mutation");
    Reject("settings_failed", [&] { fixture.Mutate("settings.update", event, Object({{"date", String("2026-02-30")}})); });
    Reject("settings_failed", [&] { fixture.Mutate("settings.update", event, Object({{"reminderMinutes", Number(4)}})); });
    Check(fixture.service.EventById(event.id)->date == "2026-10-29", "invalid date left persisted record unchanged");
    Reject("confirmation_required", [&] { fixture.Mutate("settings.action", event, Object(), "remove"); });
    fixture.Mutate("settings.action", event, Object(), "remove", true);
    Check(!fixture.service.EventById(event.id), "confirmed removal reaches real isolated calendar service");
    Reject("settings_target_not_found", [&] { fixture.Get(event); });
}

void Recurrence()
{
    Fixture fixture;
    const Target all{"calendar", {}};
    auto days = Array(); days.array.push_back(Number(5));
    const auto created = fixture.Mutate("settings.action", all, Object({
        {"event", Object({{"title", String("Weekly demo")}, {"allDay", Boolean(true)}})},
        {"rule", Object({{"kind", String("weekly")}, {"startDate", String("2026-10-29")}, {"weekdays", days}})}}), "create-series");
    const Target series{"calendar-series", created.object.at("id").string};
    const Target occurrence{"calendar-event", series.id + "/2026-10-29"};
    Check(fixture.Get(series).object.at("values").object.at("rule").object.at("kind").string == "weekly", "real recurring rule persisted");
    fixture.Mutate("settings.update", occurrence, Object({{"title", String("Specific capture")}}));
    Check(fixture.service.EventById(occurrence.id)->title == "Specific capture", "occurrence edit creates native exception");
    Check(fixture.service.SeriesById(series.id)->event.title == "Weekly demo", "occurrence edit preserves base series");
    Reject("invalid_params", [&] { fixture.Mutate("settings.update", series, Object({{"exceptions", Object()}})); });
    fixture.Mutate("settings.update", series, Object({{"event", Object({{"title", String("Scheduled demo")}})}}));
    Check(fixture.service.SeriesById(series.id)->event.title == "Scheduled demo", "series patch preserves native validation");
    fixture.Mutate("settings.action", series, Object(), "remove", true);
    Check(!fixture.service.SeriesById(series.id), "confirmed series removal");
    auto read = Object({{"target", TargetJson(all)}, {"limit", Number(0)}});
    Reject("invalid_params", [&] { HandleCalendarSettings("settings.get", read, all, fixture.context); });
}
}
int main()
{
    Events(); Recurrence();
    if (failures) return 1;
    std::cout << "Calendar settings adapter checks passed\n";
    return 0;
}
