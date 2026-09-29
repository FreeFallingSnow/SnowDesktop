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
constexpr std::size_t kMaximumSeries = 2000;
constexpr std::size_t kMaximumExceptions = 2000;
constexpr std::size_t kMaximumFileBytes =
    32u * 1024u * 1024u;
constexpr std::size_t kMaximumTitleBytes = 512;
constexpr std::size_t kMaximumNotesBytes = 8192;

bool ExceptionLimitReached(const std::vector<CalendarSeries>& series)
{
    std::size_t count = 0;
    for (const auto& item : series)
    {
        count += item.exceptions.size();
        if (count >= kMaximumExceptions) return true;
    }
    return false;
}

std::filesystem::path SeriesPath(const std::filesystem::path& legacy)
{
    return legacy.parent_path() /
        (legacy.stem().wstring() + L"-series.json");
}

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
    series_.clear();
    selectedDate_ = clock_().date;
    if (!GetDateInfo(selectedDate_))
        selectedDate_ = CurrentLocalNow().date;
    startupCheckPending_ = true;
    selectedTracksToday_ = true;
    lastCheckAbsoluteMinute_ = 0;
    nextReminderCheck_ = {};

    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error))
        return LoadSeries();
    const std::uintmax_t size =
        std::filesystem::file_size(path_, error);
    if (error || size > kMaximumFileBytes)
    {
        QuarantineCorruptFile();
        (void)LoadSeries();
        return false;
    }
    std::ifstream file(path_, std::ios::binary);
    if (!file)
    {
        (void)LoadSeries();
        return false;
    }
    std::ostringstream input;
    input << file.rdbuf();
    file.close();
    if (LoadText(input.str()))
        return LoadSeries();
    QuarantineCorruptFile();
    events_.clear();
    (void)LoadSeries();
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

std::optional<CalendarEvent> CalendarService::EventById(const std::string& id) const
{
    for (const auto& event : events_)
        if (event.id == id) return event;
    const auto separator = id.find('/');
    if (separator != std::string::npos)
        for (const auto& series : series_)
            if (series.id == id.substr(0, separator))
                return Occurrence(series, id.substr(separator + 1));
    return std::nullopt;
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
    const auto from = GetDateInfo(fromDate);
    const auto to = GetDateInfo(toDate);
    if (from && to &&
        DaysFromCivil(to->year, to->month, to->day) -
            DaysFromCivil(from->year, from->month, from->day) < 366)
    {
        for (const auto& series : series_)
        {
            for (std::string date = fromDate; date <= toDate;)
            {
                if (auto event = Occurrence(series, date);
                    event && event->date >= fromDate && event->date <= toDate)
                    result.push_back(std::move(*event));
                const auto next = AddDays(date, 1);
                if (!next) break;
                date = *next;
            }
            for (const auto& [origin, exception] : series.exceptions)
            {
                if (origin >= fromDate && origin <= toDate || exception.canceled ||
                    exception.event.date < fromDate || exception.event.date > toDate)
                    continue;
                if (auto event = Occurrence(series, origin))
                    result.push_back(std::move(*event));
            }
        }
    }
    std::sort(result.begin(), result.end(), EventLess);
    return result;
}

std::vector<CalendarEvent> CalendarService::SingleEvents() const
{
    auto result = events_;
    std::sort(result.begin(), result.end(), EventLess);
    return result;
}

bool CalendarService::MatchesRule(const CalendarSeriesRule& rule,
    const std::string& date)
{
    return Matches(rule, date);
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
    if (const auto separator = id.find('/'); separator != std::string::npos)
    {
        for (auto& series : series_)
        {
            if (series.id != id.substr(0, separator)) continue;
            const std::string origin = id.substr(separator + 1);
            if (!Occurrence(series, origin)) return {false, id, 0, "not_found"};
            if (series.revision != expectedRevision)
                return {false, id, series.revision, "conflict"};
            std::string error;
            if (!ValidateAndNormalize(event, error))
                return {false, id, series.revision, error};
            if (!series.exceptions.contains(origin) && ExceptionLimitReached(series_))
                return {false, id, series.revision, "event_limit"};
            auto previous = series;
            event.id = id;
            event.seriesId = series.id;
            event.occurrenceDate = origin;
            event.occurrenceOverride = true;
            event.revision = ++series.revision;
            event.notifiedTrigger.clear();
            series.exceptions[origin] = {false, std::move(event)};
            series.notifiedTriggers.erase(origin);
            if (!SaveSeries()) { series = std::move(previous); return {false, id, 0, "save_failed"}; }
            if (changedCallback_) changedCallback_("events");
            return {true, id, series.revision, {}};
        }
        return {false, id, 0, "not_found"};
    }
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
    const std::string& id, int expectedRevision)
{
    if (const auto separator = id.find('/'); separator != std::string::npos)
    {
        for (auto& series : series_)
        {
            if (series.id != id.substr(0, separator)) continue;
            const std::string origin = id.substr(separator + 1);
            if (!Occurrence(series, origin)) return {false, id, 0, "not_found"};
            if (expectedRevision && expectedRevision != series.revision)
                return {false, id, series.revision, "conflict"};
            if (!series.exceptions.contains(origin) && ExceptionLimitReached(series_))
                return {false, id, series.revision, "event_limit"};
            auto previous = series;
            ++series.revision;
            series.exceptions[origin] = {true, {}};
            series.notifiedTriggers.erase(origin);
            if (!SaveSeries()) { series = std::move(previous); return {false, id, 0, "save_failed"}; }
            if (changedCallback_) changedCallback_("events");
            return {true, id, 0, {}};
        }
        return {false, id, 0, "not_found"};
    }
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

std::string CalendarService::OccurrenceId(const std::string& seriesId,
    const std::string& date)
{
    return seriesId + "/" + date;
}

bool CalendarService::Matches(const CalendarSeriesRule& rule,
    const std::string& date)
{
    if (!GetDateInfo(date)) return false;
    if (rule.kind == "dates")
        return std::binary_search(rule.dates.begin(), rule.dates.end(), date);
    if (date < rule.startDate || (!rule.endDate.empty() && date > rule.endDate))
        return false;
    const auto current = *GetDateInfo(date);
    const auto start = *GetDateInfo(rule.startDate);
    if (rule.kind == "weekly")
    {
        const long long currentDay = DaysFromCivil(current.year, current.month, current.day);
        const long long startDay = DaysFromCivil(start.year, start.month, start.day);
        const long long anchor = startDay - (start.weekday + 5) % 7;
        return ((currentDay - anchor) / 7) % rule.interval == 0 &&
            std::find(rule.weekdays.begin(), rule.weekdays.end(), current.weekday) !=
                rule.weekdays.end();
    }
    if (rule.kind == "monthly")
    {
        const int months = (current.year - start.year) * 12 + current.month - start.month;
        return months % rule.interval == 0 &&
            current.day == (rule.monthDay == 0 ? current.daysInMonth : rule.monthDay);
    }
    return false;
}

std::optional<CalendarEvent> CalendarService::Occurrence(
    const CalendarSeries& series, const std::string& date) const
{
    if (!Matches(series.rule, date)) return std::nullopt;
    const auto found = series.exceptions.find(date);
    if (found != series.exceptions.end() && found->second.canceled)
        return std::nullopt;
    CalendarEvent event = found != series.exceptions.end()
        ? found->second.event : series.event;
    event.id = OccurrenceId(series.id, date);
    event.revision = series.revision;
    event.seriesId = series.id;
    event.occurrenceDate = date;
    event.occurrenceOverride = found != series.exceptions.end();
    if (!event.occurrenceOverride) event.date = date;
    return event;
}

std::optional<CalendarSeries> CalendarService::SeriesById(const std::string& id) const
{
    for (const auto& series : series_)
        if (series.id == id) return series;
    return std::nullopt;
}

bool CalendarService::ValidateSeries(CalendarSeries& series, std::string& error) const
{
    auto& rule = series.rule;
    if (rule.kind == "dates")
    {
        if (rule.dates.empty() || rule.dates.size() > 366)
        { error = "invalid_rule"; return false; }
        std::sort(rule.dates.begin(), rule.dates.end());
        if (std::adjacent_find(rule.dates.begin(), rule.dates.end()) != rule.dates.end() ||
            std::any_of(rule.dates.begin(), rule.dates.end(),
                [](const auto& date) { return !GetDateInfo(date); }))
        { error = "invalid_rule"; return false; }
        rule.startDate = rule.dates.front();
        rule.endDate = rule.dates.back();
        rule.interval = 1;
        rule.weekdays.clear();
        rule.monthDay = 0;
    }
    else if (rule.kind == "weekly" || rule.kind == "monthly")
    {
        if (!GetDateInfo(rule.startDate) ||
            (!rule.endDate.empty() &&
                (!GetDateInfo(rule.endDate) || rule.endDate < rule.startDate)) ||
            rule.interval < 1 || rule.interval > 99 || !rule.dates.empty())
        { error = "invalid_rule"; return false; }
        if (rule.kind == "weekly")
        {
            std::sort(rule.weekdays.begin(), rule.weekdays.end());
            if (rule.weekdays.empty() || rule.weekdays.size() > 7 || rule.monthDay != 0 ||
                rule.weekdays.front() < 1 || rule.weekdays.back() > 7 ||
                std::adjacent_find(rule.weekdays.begin(), rule.weekdays.end()) != rule.weekdays.end())
            { error = "invalid_rule"; return false; }
        }
        else if (!rule.weekdays.empty() || rule.monthDay < 0 || rule.monthDay > 31)
        { error = "invalid_rule"; return false; }
    }
    else { error = "invalid_rule"; return false; }
    series.event.date = rule.startDate;
    return ValidateAndNormalize(series.event, error);
}

MutationResult CalendarService::CreateSeries(CalendarSeries series)
{
    if (series_.size() >= kMaximumSeries)
        return {false, {}, 0, "event_limit"};
    std::string error;
    if (!ValidateSeries(series, error)) return {false, {}, 0, error};
    series.id = GenerateId();
    if (series.id.empty()) return {false, {}, 0, "id_failed"};
    series.revision = 1;
    series.exceptions.clear();
    series.notifiedTriggers.clear();
    series_.push_back(std::move(series));
    if (!SaveSeries()) { series_.pop_back(); return {false, {}, 0, "save_failed"}; }
    if (changedCallback_) changedCallback_("events");
    return {true, series_.back().id, 1, {}};
}

MutationResult CalendarService::UpdateSeries(const std::string& id,
    int expectedRevision, CalendarSeries series)
{
    const auto found = std::find_if(series_.begin(), series_.end(),
        [&](const auto& current) { return current.id == id; });
    if (found == series_.end()) return {false, id, 0, "not_found"};
    if (found->revision != expectedRevision)
        return {false, id, found->revision, "conflict"};
    std::string error;
    if (!ValidateSeries(series, error)) return {false, id, found->revision, error};
    CalendarSeries previous = *found;
    series.id = id;
    series.revision = found->revision + 1;
    series.exceptions.clear();
    series.notifiedTriggers.clear();
    for (const auto& [date, exception] : found->exceptions)
        if (Matches(series.rule, date)) series.exceptions.emplace(date, exception);
    const bool scheduleChanged = series.rule.kind != found->rule.kind ||
        series.rule.dates != found->rule.dates ||
        series.rule.startDate != found->rule.startDate ||
        series.rule.endDate != found->rule.endDate ||
        series.rule.interval != found->rule.interval ||
        series.rule.weekdays != found->rule.weekdays ||
        series.rule.monthDay != found->rule.monthDay ||
        series.event.allDay != found->event.allDay ||
        series.event.startMinutes != found->event.startMinutes ||
        series.event.endMinutes != found->event.endMinutes ||
        series.event.reminderMinutes != found->event.reminderMinutes;
    if (!scheduleChanged) series.notifiedTriggers = found->notifiedTriggers;
    *found = std::move(series);
    if (!SaveSeries()) { *found = std::move(previous); return {false, id, 0, "save_failed"}; }
    if (changedCallback_) changedCallback_("events");
    return {true, id, found->revision, {}};
}

MutationResult CalendarService::RemoveSeries(const std::string& id,
    int expectedRevision)
{
    const auto found = std::find_if(series_.begin(), series_.end(),
        [&](const auto& current) { return current.id == id; });
    if (found == series_.end()) return {false, id, 0, "not_found"};
    if (found->revision != expectedRevision)
        return {false, id, found->revision, "conflict"};
    const auto index = static_cast<std::size_t>(found - series_.begin());
    CalendarSeries previous = *found;
    series_.erase(found);
    if (!SaveSeries())
    { series_.insert(series_.begin() + static_cast<std::ptrdiff_t>(index), std::move(previous));
      return {false, id, 0, "save_failed"}; }
    if (changedCallback_) changedCallback_("events");
    return {true, id, 0, {}};
}

bool CalendarService::LoadSeries()
{
    series_.clear();
    const auto path = SeriesPath(path_);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return true;
    const auto size = std::filesystem::file_size(path, error);
    bool valid = !error && size <= kMaximumFileBytes;
    std::ifstream file(path, std::ios::binary);
    std::ostringstream input;
    if (valid && file) input << file.rdbuf();
    else valid = false;
    file.close();
    JsonValue root;
    valid = valid && ParseJson(input.str(), root) && root.IsObject();
    const auto* schema = valid ? Field(root, "schemaVersion", JsonValue::Type::Number) : nullptr;
    const auto* items = valid ? Field(root, "series", JsonValue::Type::Array) : nullptr;
    valid = schema && schema->number == 1 && items && items->array.size() <= kMaximumSeries;
    auto readInt = [](const JsonValue& object, const char* key, int low, int high,
        int& target) {
        const auto* field = Field(object, key, JsonValue::Type::Number);
        if (!field || !std::isfinite(field->number) ||
            std::trunc(field->number) != field->number ||
            field->number < low || field->number > high) return false;
        target = static_cast<int>(field->number);
        return true;
    };
    const auto readEvent = [&](const JsonValue& object, CalendarEvent& event) {
        const auto* title = Field(object, "title", JsonValue::Type::String);
        const auto* date = Field(object, "date", JsonValue::Type::String);
        const auto* notes = Field(object, "notes", JsonValue::Type::String);
        const auto* allDay = Field(object, "allDay", JsonValue::Type::Boolean);
        if (!title || !date || !notes || !allDay ||
            !readInt(object, "startMinutes", 0, 1439, event.startMinutes) ||
            !readInt(object, "endMinutes", 0, 1439, event.endMinutes) ||
            !readInt(object, "reminderMinutes", -1, 1440, event.reminderMinutes))
            return false;
        event.title = title->string; event.date = date->string;
        event.notes = notes->string; event.allDay = allDay->boolean;
        std::string failure;
        return ValidateAndNormalize(event, failure);
    };
    std::unordered_set<std::string> ids;
    std::size_t exceptionCount = 0;
    if (valid) for (const auto& item : items->array)
    {
        if (!item.IsObject()) { valid = false; break; }
        const auto* id = Field(item, "id", JsonValue::Type::String);
        const auto* base = Field(item, "event", JsonValue::Type::Object);
        const auto* rule = Field(item, "rule", JsonValue::Type::Object);
        const auto* exceptions = Field(item, "exceptions", JsonValue::Type::Array);
        const auto* notified = Field(item, "notified", JsonValue::Type::Array);
        CalendarSeries series;
        if (!id || id->string.empty() || id->string.find('/') != std::string::npos ||
            !ids.insert(id->string).second || !base || !rule || !exceptions || !notified ||
            !readInt(item, "revision", 1, (std::numeric_limits<int>::max)(), series.revision) ||
            !readEvent(*base, series.event)) { valid = false; break; }
        series.id = id->string;
        const auto* kind = Field(*rule, "kind", JsonValue::Type::String);
        const auto* start = Field(*rule, "startDate", JsonValue::Type::String);
        const auto* end = Field(*rule, "endDate", JsonValue::Type::String);
        const auto* dates = Field(*rule, "dates", JsonValue::Type::Array);
        const auto* weekdays = Field(*rule, "weekdays", JsonValue::Type::Array);
        if (!kind || !start || !end || !dates || !weekdays ||
            !readInt(*rule, "interval", 1, 99, series.rule.interval) ||
            !readInt(*rule, "monthDay", 0, 31, series.rule.monthDay))
        { valid = false; break; }
        series.rule.kind = kind->string;
        series.rule.startDate = start->string;
        series.rule.endDate = end->string;
        for (const auto& value : dates->array)
        { if (!value.IsString()) { valid = false; break; } series.rule.dates.push_back(value.string); }
        for (const auto& value : weekdays->array)
        { if (!value.IsNumber() || value.number < 1 || value.number > 7 ||
              std::trunc(value.number) != value.number) { valid = false; break; }
          series.rule.weekdays.push_back(static_cast<int>(value.number)); }
        std::string failure;
        if (!valid || !ValidateSeries(series, failure) ||
            exceptions->array.size() + exceptionCount > kMaximumExceptions)
        { valid = false; break; }
        exceptionCount += exceptions->array.size();
        for (const auto& exception : exceptions->array)
        {
            const auto* date = Field(exception, "date", JsonValue::Type::String);
            const auto* canceled = Field(exception, "canceled", JsonValue::Type::Boolean);
            if (!date || !canceled || !Matches(series.rule, date->string) ||
                series.exceptions.contains(date->string)) { valid = false; break; }
            CalendarSeriesException entry;
            entry.canceled = canceled->boolean;
            if (!entry.canceled)
            {
                const auto* value = Field(exception, "event", JsonValue::Type::Object);
                if (!value || !readEvent(*value, entry.event)) { valid = false; break; }
            }
            series.exceptions.emplace(date->string, std::move(entry));
        }
        if (!valid) break;
        for (const auto& entry : notified->array)
        {
            const auto* date = Field(entry, "date", JsonValue::Type::String);
            const auto* trigger = Field(entry, "trigger", JsonValue::Type::String);
            if (!date || !trigger || !Matches(series.rule, date->string) ||
                trigger->string.size() > 64 ||
                !series.notifiedTriggers.emplace(date->string, trigger->string).second)
            { valid = false; break; }
        }
        if (!valid) break;
        series_.push_back(std::move(series));
    }
    if (valid) return true;
    series_.clear();
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t suffix[64]{};
    swprintf_s(suffix, L".corrupt-%04u%02u%02u-%02u%02u%02u.json",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    MoveFileExW(path.c_str(), (path.wstring() + suffix).c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return false;
}

bool CalendarService::SaveSeries() const
{
    const auto path = SeriesPath(path_);
    std::ostringstream output;
    const auto quote = [&](std::string_view value) {
        output << '"' << EscapeJson(value) << '"';
    };
    const auto writeEvent = [&](const CalendarEvent& event) {
        output << "{\"title\":"; quote(event.title);
        output << ",\"date\":"; quote(event.date);
        output << ",\"allDay\":" << (event.allDay ? "true" : "false")
            << ",\"startMinutes\":" << event.startMinutes
            << ",\"endMinutes\":" << event.endMinutes
            << ",\"notes\":"; quote(event.notes);
        output << ",\"reminderMinutes\":" << event.reminderMinutes << '}';
    };
    output << "{\"schemaVersion\":1,\"series\":[";
    bool firstSeries = true;
    for (const auto& series : series_)
    {
        if (!firstSeries) output << ',';
        firstSeries = false;
        output << "{\"id\":"; quote(series.id);
        output << ",\"revision\":" << series.revision << ",\"event\":";
        writeEvent(series.event);
        const auto& rule = series.rule;
        output << ",\"rule\":{\"kind\":"; quote(rule.kind);
        output << ",\"startDate\":"; quote(rule.startDate);
        output << ",\"endDate\":"; quote(rule.endDate);
        output << ",\"interval\":" << rule.interval
            << ",\"monthDay\":" << rule.monthDay << ",\"dates\":[";
        for (std::size_t i = 0; i < rule.dates.size(); ++i)
        { if (i) output << ','; quote(rule.dates[i]); }
        output << "],\"weekdays\":[";
        for (std::size_t i = 0; i < rule.weekdays.size(); ++i)
        { if (i) output << ','; output << rule.weekdays[i]; }
        output << "]},\"exceptions\":[";
        bool first = true;
        for (const auto& [date, exception] : series.exceptions)
        {
            if (!first) output << ',';
            first = false;
            output << "{\"date\":"; quote(date);
            output << ",\"canceled\":" << (exception.canceled ? "true" : "false");
            if (!exception.canceled) { output << ",\"event\":"; writeEvent(exception.event); }
            output << '}';
        }
        output << "],\"notified\":[";
        first = true;
        for (const auto& [date, trigger] : series.notifiedTriggers)
        {
            if (!first) output << ',';
            first = false;
            output << "{\"date\":"; quote(date);
            output << ",\"trigger\":"; quote(trigger);
            output << '}';
        }
        output << "]}";
    }
    output << "]}\n";
    const auto text = output.str();
    if (text.size() > kMaximumFileBytes) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) return false;
    file.close();
    return MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
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
    const long long previousCheck = lastCheckAbsoluteMinute_;

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
    if (!due.empty() && !Save())
    {
        events_ = std::move(previous);
        due.clear();
    }
    if (notificationCallback_)
    {
        for (const std::size_t index : due)
            notificationCallback_(events_[index]);
    }
    if (series_.empty()) return;
    const auto tomorrow = AddDays(now.date, 1);
    if (!tomorrow) return;
    const auto candidates = Events(now.date, *tomorrow);
    std::vector<CalendarEvent> seriesDue;
    const auto previousSeries = series_;
    bool changed = false;
    const auto yesterday = AddDays(now.date, -1).value_or(now.date);
    for (auto& series : series_)
        for (auto it = series.notifiedTriggers.begin(); it != series.notifiedTriggers.end();)
            if (it->second.substr(0, 10) < yesterday)
            { it = series.notifiedTriggers.erase(it); changed = true; }
            else ++it;
    for (const auto& event : candidates)
    {
        if (event.seriesId.empty() || event.reminderMinutes < 0) continue;
        const int baseMinutes = event.allDay ? 9 * 60 : event.startMinutes;
        const long long start = AbsoluteMinute(event.date, baseMinutes);
        const long long end = AbsoluteMinute(event.date,
            event.allDay ? 1439 : event.endMinutes);
        const long long trigger = start - event.reminderMinutes;
        if (end < absoluteNow || trigger > absoluteNow ||
            (startupCatchUp ? trigger < todayStart : trigger <= previousCheck))
            continue;
        const auto found = std::find_if(series_.begin(), series_.end(),
            [&](const auto& series) { return series.id == event.seriesId; });
        if (found == series_.end()) continue;
        const auto key = TriggerKey(trigger);
        if (found->notifiedTriggers[event.occurrenceDate] == key) continue;
        found->notifiedTriggers[event.occurrenceDate] = key;
        changed = true;
        seriesDue.push_back(event);
    }
    if (changed && !SaveSeries())
    { series_ = previousSeries; seriesDue.clear(); }
    if (notificationCallback_)
        for (const auto& event : seriesDue) notificationCallback_(event);
}

} // namespace snowdesktop::calendar
