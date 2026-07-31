#include "calendar_service.h"

#include "json_value.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace snowdesktop::calendar
{
namespace
{
constexpr int kSchemaVersion = 1;
constexpr std::size_t kMaximumEvents = 2000;
constexpr std::size_t kMaximumFileBytes =
    32u * 1024u * 1024u;
constexpr std::size_t kMaximumTitleBytes = 512;
constexpr std::size_t kMaximumNotesBytes = 8192;

long long DaysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(
        year - era * 400);
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) /
            5 +
        day - 1;
    const unsigned doe =
        yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 +
        static_cast<long long>(doe) - 719468;
}

DateInfo CivilFromDays(long long days)
{
    days += 719468;
    const long long era =
        (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(
        days - era * 146097);
    const unsigned yoe =
        (doe - doe / 1460 + doe / 36524 -
            doe / 146096) /
        365;
    int year = static_cast<int>(yoe) +
        static_cast<int>(era) * 400;
    const unsigned doy =
        doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day =
        doy - (153 * mp + 2) / 5 + 1;
    const int month =
        static_cast<int>(mp) + (mp < 10 ? 3 : -9);
    year += month <= 2;
    DateInfo result;
    result.year = year;
    result.month = month;
    result.day = static_cast<int>(day);
    return result;
}

int DaysInMonth(int year, int month)
{
    static constexpr std::array<int, 12> days = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12)
        return 0;
    if (month != 2)
        return days[static_cast<std::size_t>(month - 1)];
    const bool leap =
        year % 4 == 0 &&
        (year % 100 != 0 || year % 400 == 0);
    return leap ? 29 : 28;
}

bool ParseDate(
    const std::string& value, DateInfo& output)
{
    if (value.size() != 10 ||
        value[4] != '-' || value[7] != '-')
        return false;
    for (std::size_t index = 0;
        index < value.size(); ++index)
    {
        if (index == 4 || index == 7)
            continue;
        if (!std::isdigit(
                static_cast<unsigned char>(value[index])))
            return false;
    }
    const int year = std::stoi(value.substr(0, 4));
    const int month = std::stoi(value.substr(5, 2));
    const int day = std::stoi(value.substr(8, 2));
    const int monthDays = DaysInMonth(year, month);
    if (year < 1 || year > 9999 ||
        monthDays == 0 || day < 1 || day > monthDays)
        return false;
    output.year = year;
    output.month = month;
    output.day = day;
    output.daysInMonth = monthDays;
    const long long serial = DaysFromCivil(
        year,
        static_cast<unsigned>(month),
        static_cast<unsigned>(day));
    int weekday = static_cast<int>((serial + 4) % 7);
    if (weekday < 0)
        weekday += 7;
    output.weekday = weekday + 1;
    return true;
}

std::string FormatDate(
    int year, int month, int day)
{
    std::ostringstream output;
    output << std::setfill('0')
        << std::setw(4) << year << '-'
        << std::setw(2) << month << '-'
        << std::setw(2) << day;
    return output.str();
}

std::string FormatDate(const DateInfo& date)
{
    return FormatDate(date.year, date.month, date.day);
}

std::string EscapeJson(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                result += "\\u00";
                result.push_back(hex[(ch >> 4) & 0x0f]);
                result.push_back(hex[ch & 0x0f]);
            }
            else
            {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

std::string Trim(std::string value)
{
    while (!value.empty() &&
        std::isspace(
            static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() &&
        std::isspace(
            static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

const JsonValue* Field(
    const JsonValue& object, std::string_view key,
    JsonValue::Type type)
{
    const JsonValue* value = object.Find(key);
    return value && value->type == type ? value : nullptr;
}

bool IsAllowedReminder(int minutes)
{
    static constexpr std::array<int, 7> values = {
        -1, 0, 5, 15, 30, 60, 1440,
    };
    return std::find(values.begin(), values.end(), minutes) !=
        values.end();
}

bool EventLess(
    const CalendarEvent& left,
    const CalendarEvent& right)
{
    if (left.date != right.date)
        return left.date < right.date;
    if (left.allDay != right.allDay)
        return left.allDay;
    if (left.startMinutes != right.startMinutes)
        return left.startMinutes < right.startMinutes;
    if (left.title != right.title)
        return left.title < right.title;
    return left.id < right.id;
}

long long AbsoluteMinute(
    const std::string& date, int minutes)
{
    DateInfo info;
    if (!ParseDate(date, info))
        return std::numeric_limits<long long>::min();
    return DaysFromCivil(
               info.year,
               static_cast<unsigned>(info.month),
               static_cast<unsigned>(info.day)) *
            1440 +
        minutes;
}

std::string TriggerKey(long long absoluteMinute)
{
    long long day = absoluteMinute / 1440;
    int minutes = static_cast<int>(absoluteMinute % 1440);
    if (minutes < 0)
    {
        minutes += 1440;
        --day;
    }
    const DateInfo date = CivilFromDays(day);
    return FormatDate(date) + "@" +
        std::to_string(minutes);
}
}

CalendarService::CalendarService(
    std::filesystem::path path, Clock clock)
    : path_(std::move(path)),
      clock_(clock ? std::move(clock) : CurrentLocalNow)
{
}

CalendarNow CalendarService::CurrentLocalNow()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return {
        FormatDate(now.wYear, now.wMonth, now.wDay),
        static_cast<int>(now.wHour) * 60 +
            static_cast<int>(now.wMinute),
    };
}

std::optional<DateInfo> CalendarService::GetDateInfo(
    const std::string& date)
{
    DateInfo result;
    if (!ParseDate(date, result))
        return std::nullopt;
    return result;
}

std::optional<std::string> CalendarService::AddDays(
    const std::string& date, int offset)
{
    DateInfo parsed;
    if (!ParseDate(date, parsed))
        return std::nullopt;
    const long long serial = DaysFromCivil(
        parsed.year,
        static_cast<unsigned>(parsed.month),
        static_cast<unsigned>(parsed.day));
    const DateInfo shifted =
        CivilFromDays(serial + offset);
    if (shifted.year < 1 || shifted.year > 9999)
        return std::nullopt;
    return FormatDate(shifted);
}

bool CalendarService::Load()
{
    events_.clear();
    selectedDate_ = clock_().date;
    if (!GetDateInfo(selectedDate_))
        selectedDate_ = CurrentLocalNow().date;
    startupCheckPending_ = true;
    selectedTracksToday_ = true;
    lastCheckAbsoluteMinute_ = 0;
    nextReminderCheck_ = {};

    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error))
        return true;
    const std::uintmax_t size =
        std::filesystem::file_size(path_, error);
    if (error || size > kMaximumFileBytes)
    {
        QuarantineCorruptFile();
        return false;
    }
    std::ifstream file(path_, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream input;
    input << file.rdbuf();
    file.close();
    if (LoadText(input.str()))
        return true;
    QuarantineCorruptFile();
    events_.clear();
    return false;
}

bool CalendarService::LoadText(const std::string& text)
{
    JsonValue root;
    if (!ParseJson(text, root) || !root.IsObject())
        return false;
    const JsonValue* schema =
        Field(root, "schemaVersion", JsonValue::Type::Number);
    const JsonValue* events =
        Field(root, "events", JsonValue::Type::Array);
    if (!schema || !std::isfinite(schema->number) ||
        schema->number != kSchemaVersion ||
        !events ||
        events->array.size() > kMaximumEvents)
        return false;

    std::unordered_set<std::string> ids;
    std::vector<CalendarEvent> loaded;
    loaded.reserve(events->array.size());
    for (const JsonValue& value : events->array)
    {
        if (!value.IsObject())
            return false;
        const JsonValue* id =
            Field(value, "id", JsonValue::Type::String);
        const JsonValue* revision =
            Field(value, "revision", JsonValue::Type::Number);
        const JsonValue* title =
            Field(value, "title", JsonValue::Type::String);
        const JsonValue* date =
            Field(value, "date", JsonValue::Type::String);
        const JsonValue* allDay =
            Field(value, "allDay", JsonValue::Type::Boolean);
        const JsonValue* startMinutes =
            Field(value, "startMinutes", JsonValue::Type::Number);
        const JsonValue* endMinutes =
            Field(value, "endMinutes", JsonValue::Type::Number);
        const JsonValue* notes =
            Field(value, "notes", JsonValue::Type::String);
        const JsonValue* reminderMinutes =
            Field(value, "reminderMinutes", JsonValue::Type::Number);
        const JsonValue* notifiedTrigger =
            Field(value, "notifiedTrigger", JsonValue::Type::String);
        if (!id || !revision || !title || !date || !allDay ||
            !startMinutes || !endMinutes || !notes ||
            !reminderMinutes || !notifiedTrigger ||
            id->string.empty() || ids.contains(id->string))
            return false;
        auto exactInt = [](const JsonValue* value,
            int minimum, int maximum,
            int& output) {
            if (!value || !std::isfinite(value->number) ||
                std::trunc(value->number) != value->number ||
                value->number < minimum ||
                value->number > maximum)
            {
                return false;
            }
            output = static_cast<int>(value->number);
            return true;
        };
        CalendarEvent event;
        event.id = id->string;
        event.title = title->string;
        event.date = date->string;
        event.allDay = allDay->boolean;
        event.notes = notes->string;
        event.notifiedTrigger =
            notifiedTrigger->string;
        if (!exactInt(
                revision, 1,
                std::numeric_limits<int>::max(),
                event.revision) ||
            !exactInt(
                startMinutes, 0, 1439,
                event.startMinutes) ||
            !exactInt(
                endMinutes, 0, 1439,
                event.endMinutes) ||
            !exactInt(
                reminderMinutes, -1, 1440,
                event.reminderMinutes))
        {
            return false;
        }
        std::string validationError;
        if (!ValidateAndNormalize(
                event, validationError))
            return false;
        ids.insert(event.id);
        loaded.push_back(std::move(event));
    }
    events_ = std::move(loaded);
    return true;
}

bool CalendarService::Save() const
{
    std::vector<CalendarEvent> sorted = events_;
    std::sort(sorted.begin(), sorted.end(), EventLess);
    std::ostringstream output;
    output << "{\n  \"schemaVersion\": "
        << kSchemaVersion << ",\n  \"events\": [";
    for (std::size_t index = 0;
        index < sorted.size(); ++index)
    {
        const CalendarEvent& event = sorted[index];
        output << (index == 0 ? "\n" : ",\n")
            << "    {"
            << "\"id\":\"" << EscapeJson(event.id) << "\","
            << "\"revision\":" << event.revision << ','
            << "\"title\":\"" << EscapeJson(event.title) << "\","
            << "\"date\":\"" << EscapeJson(event.date) << "\","
            << "\"allDay\":" << (event.allDay ? "true" : "false") << ','
            << "\"startMinutes\":" << event.startMinutes << ','
            << "\"endMinutes\":" << event.endMinutes << ','
            << "\"notes\":\"" << EscapeJson(event.notes) << "\","
            << "\"reminderMinutes\":" << event.reminderMinutes << ','
            << "\"notifiedTrigger\":\""
            << EscapeJson(event.notifiedTrigger) << "\"}";
    }
    if (!sorted.empty())
        output << '\n';
    output << "  ]\n}\n";

    std::error_code error;
    std::filesystem::create_directories(
        path_.parent_path(), error);
    const std::filesystem::path temporary =
        path_.wstring() + L".tmp";
    std::ofstream file(
        temporary,
        std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    const std::string text = output.str();
    file.write(
        text.data(),
        static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file)
        return false;
    file.close();
    return MoveFileExW(
               temporary.c_str(), path_.c_str(),
               MOVEFILE_REPLACE_EXISTING |
                   MOVEFILE_WRITE_THROUGH) != FALSE;
}

void CalendarService::QuarantineCorruptFile() const
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t suffix[64]{};
    swprintf_s(
        suffix,
        L".corrupt-%04u%02u%02u-%02u%02u%02u.json",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond);
    const std::filesystem::path quarantine =
        path_.wstring() + suffix;
    MoveFileExW(
        path_.c_str(), quarantine.c_str(),
        MOVEFILE_REPLACE_EXISTING |
            MOVEFILE_WRITE_THROUGH);
}

bool CalendarService::ValidateAndNormalize(
    CalendarEvent& event, std::string& error) const
{
    event.title = Trim(std::move(event.title));
    if (event.title.empty())
    {
        error = "title_required";
        return false;
    }
    if (event.title.size() > kMaximumTitleBytes ||
        event.notes.size() > kMaximumNotesBytes)
    {
        error = "text_too_long";
        return false;
    }
    if (!GetDateInfo(event.date))
    {
        error = "invalid_date";
        return false;
    }
    if (!IsAllowedReminder(event.reminderMinutes))
    {
        error = "invalid_reminder";
        return false;
    }
    if (event.allDay)
    {
        event.startMinutes = 0;
        event.endMinutes = 1439;
        return true;
    }
    if (event.startMinutes < 0 ||
        event.startMinutes > 1439 ||
        event.endMinutes < event.startMinutes ||
        event.endMinutes > 1439)
    {
        error = "invalid_time";
        return false;
    }
    return true;
}

std::vector<CalendarEvent> CalendarService::Events(
    const std::string& fromDate,
    const std::string& toDate) const
{
    if (!GetDateInfo(fromDate) ||
        !GetDateInfo(toDate) ||
        fromDate > toDate)
        return {};
    std::vector<CalendarEvent> result;
    for (const CalendarEvent& event : events_)
    {
        if (event.date >= fromDate &&
            event.date <= toDate)
            result.push_back(event);
    }
    std::sort(result.begin(), result.end(), EventLess);
    return result;
}

bool CalendarService::SetSelectedDate(
    const std::string& date)
{
    if (!GetDateInfo(date))
        return false;
    if (selectedDate_ == date)
    {
        selectedTracksToday_ =
            date == clock_().date;
        return true;
    }
    selectedDate_ = date;
    selectedTracksToday_ =
        date == clock_().date;
    if (changedCallback_)
        changedCallback_("selection");
    return true;
}

MutationResult CalendarService::Create(
    CalendarEvent event)
{
    if (events_.size() >= kMaximumEvents)
        return { false, {}, 0, "event_limit" };
    std::string error;
    if (!ValidateAndNormalize(event, error))
        return { false, {}, 0, error };
    event.id = GenerateId();
    if (event.id.empty())
        return { false, {}, 0, "id_failed" };
    event.revision = 1;
    event.notifiedTrigger.clear();
    events_.push_back(event);
    if (!Save())
    {
        events_.pop_back();
        return { false, {}, 0, "save_failed" };
    }
    if (changedCallback_)
        changedCallback_("events");
    return { true, event.id, event.revision, {} };
}

MutationResult CalendarService::Update(
    const std::string& id,
    int expectedRevision,
    CalendarEvent event)
{
    const auto found = std::find_if(
        events_.begin(), events_.end(),
        [&](const CalendarEvent& current) {
            return current.id == id;
        });
    if (found == events_.end())
        return { false, id, 0, "not_found" };
    if (found->revision != expectedRevision)
        return {
            false, id, found->revision, "conflict"
        };
    std::string error;
    if (!ValidateAndNormalize(event, error))
        return { false, id, found->revision, error };
    event.id = found->id;
    event.revision = found->revision + 1;
    const bool scheduleChanged =
        event.date != found->date ||
        event.allDay != found->allDay ||
        event.startMinutes != found->startMinutes ||
        event.endMinutes != found->endMinutes ||
        event.reminderMinutes != found->reminderMinutes;
    event.notifiedTrigger = scheduleChanged
        ? std::string()
        : found->notifiedTrigger;
    CalendarEvent previous = *found;
    *found = event;
    if (!Save())
    {
        *found = std::move(previous);
        return {
            false, id, found->revision, "save_failed"
        };
    }
    if (changedCallback_)
        changedCallback_("events");
    return { true, id, event.revision, {} };
}

MutationResult CalendarService::Remove(
    const std::string& id)
{
    const auto found = std::find_if(
        events_.begin(), events_.end(),
        [&](const CalendarEvent& event) {
            return event.id == id;
        });
    if (found == events_.end())
        return { false, id, 0, "not_found" };
    const std::size_t index =
        static_cast<std::size_t>(found - events_.begin());
    CalendarEvent previous = *found;
    events_.erase(found);
    if (!Save())
    {
        events_.insert(
            events_.begin() +
                static_cast<std::ptrdiff_t>(index),
            std::move(previous));
        return { false, id, 0, "save_failed" };
    }
    if (changedCallback_)
        changedCallback_("events");
    return { true, id, 0, {} };
}

std::string CalendarService::GenerateId()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        return {};
    wchar_t value[40]{};
    if (StringFromGUID2(
            guid, value,
            static_cast<int>(std::size(value))) <= 0)
        return {};
    std::wstring wide(value);
    if (!wide.empty() && wide.front() == L'{')
        wide.erase(wide.begin());
    if (!wide.empty() && wide.back() == L'}')
        wide.pop_back();
    std::string result;
    result.reserve(wide.size());
    for (const wchar_t ch : wide)
        result.push_back(static_cast<char>(
            ch >= L'A' && ch <= L'Z'
                ? ch - L'A' + L'a'
                : ch));
    return result;
}

void CalendarService::Tick()
{
    const auto steadyNow =
        std::chrono::steady_clock::now();
    if (nextReminderCheck_.time_since_epoch().count() != 0 &&
        steadyNow < nextReminderCheck_)
        return;
    nextReminderCheck_ =
        steadyNow + std::chrono::seconds(30);
    const CalendarNow now = clock_();
    if (selectedTracksToday_ &&
        SelectedDate() != now.date &&
        GetDateInfo(now.date))
    {
        selectedDate_ = now.date;
        if (changedCallback_)
            changedCallback_("selection");
    }
    CheckReminders(now, startupCheckPending_);
    startupCheckPending_ = false;
}

void CalendarService::CheckReminders(
    const CalendarNow& now, bool startupCatchUp)
{
    if (!GetDateInfo(now.date) ||
        now.minutes < 0 || now.minutes > 1439)
        return;
    const long long absoluteNow =
        AbsoluteMinute(now.date, now.minutes);
    const long long todayStart =
        AbsoluteMinute(now.date, 0);
    if (lastCheckAbsoluteMinute_ == 0)
        lastCheckAbsoluteMinute_ = absoluteNow - 1;

    std::vector<std::size_t> due;
    std::vector<CalendarEvent> previous = events_;
    for (std::size_t index = 0;
        index < events_.size(); ++index)
    {
        CalendarEvent& event = events_[index];
        if (event.reminderMinutes < 0)
            continue;
        const int baseMinutes =
            event.allDay ? 9 * 60 : event.startMinutes;
        const long long eventStart =
            AbsoluteMinute(event.date, baseMinutes);
        const long long eventEnd =
            AbsoluteMinute(
                event.date,
                event.allDay ? 1439 : event.endMinutes);
        const long long trigger =
            eventStart - event.reminderMinutes;
        const std::string triggerKey =
            TriggerKey(trigger);
        if (event.notifiedTrigger == triggerKey ||
            eventEnd < absoluteNow)
            continue;
        bool shouldNotify = false;
        if (startupCatchUp)
        {
            shouldNotify =
                trigger >= todayStart &&
                trigger <= absoluteNow;
        }
        else
        {
            shouldNotify =
                trigger > lastCheckAbsoluteMinute_ &&
                trigger <= absoluteNow;
        }
        if (!shouldNotify)
            continue;
        event.notifiedTrigger = triggerKey;
        due.push_back(index);
    }
    lastCheckAbsoluteMinute_ = absoluteNow;
    if (due.empty())
        return;
    if (!Save())
    {
        events_ = std::move(previous);
        return;
    }
    if (notificationCallback_)
    {
        for (const std::size_t index : due)
            notificationCallback_(events_[index]);
    }
}

} // namespace snowdesktop::calendar
