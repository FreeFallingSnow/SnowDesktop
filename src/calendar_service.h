#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::calendar
{

struct CalendarNow
{
    std::string date;
    int minutes = 0;
};

struct DateInfo
{
    int year = 0;
    int month = 0;
    int day = 0;
    int weekday = 0; // 1 = Sunday
    int daysInMonth = 0;
};

struct CalendarEvent
{
    std::string id;
    int revision = 0;
    std::string title;
    std::string date;
    bool allDay = false;
    int startMinutes = 0;
    int endMinutes = 0;
    std::string notes;
    int reminderMinutes = -1;
    std::string notifiedTrigger;
};

struct MutationResult
{
    bool ok = false;
    std::string id;
    int revision = 0;
    std::string error;
};

class CalendarService
{
public:
    using Clock = std::function<CalendarNow()>;
    using ChangedCallback =
        std::function<void(const std::string&)>;
    using NotificationCallback =
        std::function<void(const CalendarEvent&)>;

    explicit CalendarService(
        std::filesystem::path path,
        Clock clock = {});

    bool Load();
    const std::string& SelectedDate() const
    {
        return selectedDate_;
    }
    bool SetSelectedDate(const std::string& date);

    static std::optional<DateInfo> GetDateInfo(
        const std::string& date);
    static std::optional<std::string> AddDays(
        const std::string& date, int offset);
    static CalendarNow CurrentLocalNow();

    std::vector<CalendarEvent> Events(
        const std::string& fromDate,
        const std::string& toDate) const;
    MutationResult Create(CalendarEvent event);
    MutationResult Update(
        const std::string& id,
        int expectedRevision,
        CalendarEvent event);
    MutationResult Remove(const std::string& id);

    void SetChangedCallback(ChangedCallback callback)
    {
        changedCallback_ = std::move(callback);
    }
    void SetNotificationCallback(
        NotificationCallback callback)
    {
        notificationCallback_ = std::move(callback);
    }

    void Tick();
    void CheckReminders(
        const CalendarNow& now, bool startupCatchUp);

private:
    bool Save() const;
    bool LoadText(const std::string& text);
    bool ValidateAndNormalize(
        CalendarEvent& event,
        std::string& error) const;
    void QuarantineCorruptFile() const;
    static std::string GenerateId();

    std::filesystem::path path_;
    Clock clock_;
    std::string selectedDate_;
    std::vector<CalendarEvent> events_;
    ChangedCallback changedCallback_;
    NotificationCallback notificationCallback_;
    bool startupCheckPending_ = true;
    bool selectedTracksToday_ = true;
    long long lastCheckAbsoluteMinute_ = 0;
    std::chrono::steady_clock::time_point nextReminderCheck_{};
};

} // namespace snowdesktop::calendar
