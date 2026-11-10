#include "calendar_ics.h"

#include <Windows.h>
#include <bcrypt.h>

extern "C" {
#include <icalcomponent.h>
#include <icalparser.h>
#include <icaltimezone.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace snowdesktop::calendar
{
namespace
{
using namespace std::chrono;
using Stamp = sys_seconds; // Calendar wall times also use this arithmetic scale.
using Component = icalcomponent;
constexpr std::size_t MaximumRows = 24000;
constexpr std::size_t MaximumOccurrences = 120000;
constexpr std::size_t MaximumExpandedText = 16 * 1024 * 1024;

struct Zone
{
    icaltimezone* embedded = nullptr;
    const time_zone* iana = nullptr;
    bool utc = false;
    bool Zoned() const { return embedded || iana || utc; }
};

struct EventTime
{
    icaltimetype value;
    Zone zone;
};

struct InvalidFeed {};

bool SameToken(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && _strnicmp(left.data(), right.data(), left.size()) == 0;
}

// The feed is untrusted. Bound the component tree before libical walks it
// recursively, and require a complete snapshot before replacing the cache.
bool HasCalendarEnvelope(std::string_view text)
{
    const auto takeLine = [&] {
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        return line;
    };
    std::array<std::string, 16> stack;
    std::size_t depth = 0;
    bool seenCalendar = false;
    while (!text.empty())
    {
        auto line = takeLine();
        if (line.find_first_not_of(" \t") == std::string_view::npos) continue;
        if (line.front() == ' ' || line.front() == '\t') return false;
        std::string unfolded;
        if (text.starts_with(' ') || text.starts_with('\t'))
        {
            unfolded = line;
            do
            {
                unfolded.append(takeLine().substr(1));
            } while (text.starts_with(' ') || text.starts_with('\t'));
            line = unfolded;
        }
        const auto separator = line.find_first_of(":;");
        const auto name = line.substr(0, separator);
        if (SameToken(name, "BEGIN") || SameToken(name, "END"))
        {
            if (separator == std::string_view::npos || line[separator] != ':') return false;
            const auto component = line.substr(separator + 1);
            if (component.empty()) return false;
            if (SameToken(name, "BEGIN"))
            {
                if (depth == stack.size() || (!depth && (seenCalendar || !SameToken(component, "VCALENDAR"))))
                    return false;
                if (!depth) seenCalendar = true;
                stack[depth++].assign(component);
            }
            else if (!depth || !SameToken(stack[--depth], component))
                return false;
        }
        else if (!depth)
            return false;
    }
    return seenCalendar && depth == 0;
}

Stamp WallTime(icaltimetype value)
{
    const year_month_day date{year{value.year}, month{static_cast<unsigned>(value.month)},
        day{static_cast<unsigned>(value.day)}};
    if (!date.ok() || value.year < 1 || value.year > 9999 || value.hour < 0 ||
        value.hour > 23 || value.minute < 0 || value.minute > 59 ||
        value.second < 0 || value.second > 60)
        throw InvalidFeed{};
    return sys_days{date} + hours{value.hour} + minutes{value.minute} + seconds{value.second};
}

icaltimetype ToIcal(Stamp value, bool allDay)
{
    const auto datePoint = floor<days>(value);
    const year_month_day date{datePoint};
    const hh_mm_ss clock{value - datePoint};
    auto result = icaltime_null_time();
    result.year = static_cast<int>(date.year());
    result.month = static_cast<int>(static_cast<unsigned>(date.month()));
    result.day = static_cast<int>(static_cast<unsigned>(date.day()));
    result.hour = static_cast<int>(clock.hours().count());
    result.minute = static_cast<int>(clock.minutes().count());
    result.second = static_cast<int>(clock.seconds().count());
    result.is_date = allDay;
    return result;
}

EventTime ReadTime(Component* calendar, icalproperty* property,
    icaltimetype value, Zone fallback = {})
{
    if (value.is_date) return {value, {}};
    if (icaltime_is_utc(value)) return {value, Zone{nullptr, nullptr, true}};
    const auto* parameter = icalproperty_get_first_parameter(property, ICAL_TZID_PARAMETER);
    if (!parameter) return {value, fallback};
    const char* id = icalparameter_get_tzid(parameter);
    if (!id || !*id) throw InvalidFeed{};
    if (auto* embedded = icalcomponent_get_timezone(calendar, id))
        return {value, Zone{embedded}};
    try
    {
        return {value, Zone{nullptr, locate_zone(id)}};
    }
    catch (const std::runtime_error&)
    {
        throw InvalidFeed{};
    }
}

Stamp Timeline(EventTime time)
{
    const auto wall = WallTime(time.value);
    if (time.zone.embedded)
    {
        int daylight = 0;
        return wall - seconds{icaltimezone_get_utc_offset(time.zone.embedded, &time.value, &daylight)};
    }
    if (time.zone.iana)
    {
        const auto local = local_seconds{wall.time_since_epoch()};
        const auto info = time.zone.iana->get_info(local);
        return wall - info.first.offset;
    }
    return wall;
}

Stamp ToDisplayTime(Stamp time, Zone zone, const time_zone* localZone)
{
    return zone.Zoned() ? Stamp{localZone->to_local(time).time_since_epoch()} : time;
}

std::string DateText(sys_days point)
{
    const year_month_day date{point};
    char text[16]{};
    std::snprintf(text, sizeof(text), "%04d-%02u-%02u",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()));
    return text;
}

std::string Id(std::string_view source, std::string_view uid, std::string_view occurrence)
{
    std::string key{source};
    key.append(1, '\0').append(uid).append(1, '\0').append(occurrence);
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(key.data()),
            static_cast<ULONG>(key.size()), digest, static_cast<ULONG>(sizeof(digest))) < 0)
        throw InvalidFeed{};
    constexpr char hex[] = "0123456789abcdef";
    std::string id = SubscriptionEventIdPrefix;
    for (const unsigned char byte : digest)
    {
        id.push_back(hex[byte >> 4]);
        id.push_back(hex[byte & 15]);
    }
    return id;
}

std::string BoundedText(const char* value, std::size_t limit)
{
    if (!value) return {};
    std::string_view text(value);
    if (text.size() <= limit) return std::string{text};
    while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80)
        --limit;
    return std::string{text.substr(0, limit)};
}

bool Cancelled(Component* event)
{
    return icalcomponent_get_status(event) == ICAL_STATUS_CANCELLED;
}

struct Group
{
    Component* master = nullptr;
    std::vector<Component*> exceptions;
};

struct Appointment
{
    EventTime start;
    seconds duration{};
    bool wallDuration = false;
    std::string title;
    std::string notes;
};

Appointment ReadAppointment(Component* calendar, Component* event, const Appointment* master = nullptr)
{
    auto* start = icalcomponent_get_first_property(event, ICAL_DTSTART_PROPERTY);
    if (!start) throw InvalidFeed{};
    Appointment appointment{ReadTime(calendar, start, icalproperty_get_dtstart(start))};
    auto* end = icalcomponent_get_first_property(event, ICAL_DTEND_PROPERTY);
    auto* duration = icalcomponent_get_first_property(event, ICAL_DURATION_PROPERTY);
    if (end)
    {
        const auto finish = ReadTime(calendar, end, icalproperty_get_dtend(end));
        if (finish.value.is_date != appointment.start.value.is_date) throw InvalidFeed{};
        appointment.duration = Timeline(finish) - Timeline(appointment.start);
    }
    else if (duration)
    {
        const auto value = icalproperty_get_duration(duration);
        appointment.duration = seconds{icaldurationtype_as_utc_seconds(value)};
        appointment.wallDuration = value.days || value.weeks;
    }
    else if (master)
    {
        appointment.duration = master->duration;
        appointment.wallDuration = master->wallDuration;
    }
    else if (appointment.start.value.is_date)
        appointment.duration = days{1};
    if (appointment.duration < seconds::zero()) throw InvalidFeed{};
    const auto* summary = icalcomponent_get_summary(event);
    const auto* description = icalcomponent_get_description(event);
    appointment.title = summary ? BoundedText(summary, 512) : master ? master->title : std::string{};
    appointment.notes = description ? BoundedText(description, 8192) : master ? master->notes : std::string{};
    return appointment;
}

struct Expansion
{
    Component* calendar;
    std::string_view source;
    sys_days first;
    sys_days end; // Exclusive.
    const time_zone* localZone;
    std::stop_token token;
    std::vector<CalendarEvent> events;
    std::size_t occurrenceCount = 0;
    std::size_t iterationCount = 0;
    std::size_t expandedText = 0;

    void Project(const Appointment& appointment, EventTime start, std::string_view uid,
        std::string_view recurrence)
    {
        if (++occurrenceCount > MaximumOccurrences || token.stop_requested()) throw InvalidFeed{};
        const auto timeline = Timeline(start);
        auto finish = timeline + appointment.duration;
        if (appointment.wallDuration)
            finish = Timeline({ToIcal(WallTime(start.value) + appointment.duration,
                start.value.is_date), start.zone});
        const auto beginLocal = ToDisplayTime(timeline, start.zone, localZone);
        const auto endLocal = std::max(beginLocal, ToDisplayTime(finish, start.zone, localZone));
        const auto originalDay = floor<days>(beginLocal);
        const auto lastDay = endLocal == beginLocal ? originalDay : floor<days>(endLocal - seconds{1});
        auto dayPoint = std::max(first, originalDay);
        if (dayPoint >= end || dayPoint > lastDay) return;

        const auto id = Id(source, uid, recurrence);
        for (; dayPoint < end && dayPoint <= lastDay; dayPoint += days{1})
        {
            expandedText += appointment.title.size() + appointment.notes.size();
            if (events.size() >= MaximumRows || expandedText > MaximumExpandedText) throw InvalidFeed{};
            CalendarEvent row;
            row.id = id + "-" + std::to_string((dayPoint - originalDay).count());
            row.title = appointment.title;
            row.notes = appointment.notes;
            row.date = DateText(dayPoint);
            row.allDay = start.value.is_date;
            row.startMinutes = static_cast<int>(floor<minutes>(std::max(beginLocal, Stamp{dayPoint}) - dayPoint).count());
            row.endMinutes = static_cast<int>(std::clamp(ceil<minutes>(endLocal - dayPoint),
                minutes{row.startMinutes}, minutes{1439}).count());
            // Remote rows are read-only. Keep their revision stable across
            // downloads that leave the visible contents unchanged.
            std::string revisionKey = row.date + ":" + std::to_string(row.startMinutes) + ":" +
                std::to_string(row.endMinutes) + (row.allDay ? ":1:" : ":0:") + row.title;
            revisionKey.append(1, '\0').append(row.notes);
            row.revision = 1 + static_cast<int>(std::hash<std::string>{}(revisionKey) % 2147483646);
            events.push_back(std::move(row));
        }
    }

    void AddGroup(const std::string& uid, const Group& group)
    {
        std::optional<Appointment> master;
        if (group.master && Cancelled(group.master)) return;
        if (group.master) master = ReadAppointment(calendar, group.master);
        const auto defaultZone = master ? master->start.zone : Zone{};

        std::map<Stamp, Component*> overrides;
        for (auto* event : group.exceptions)
        {
            auto* property = icalcomponent_get_first_property(event, ICAL_RECURRENCEID_PROPERTY);
            const auto id = Timeline(ReadTime(calendar, property, icalproperty_get_recurrenceid(property), defaultZone));
            auto& current = overrides[id];
            if (!current || icalcomponent_get_sequence(event) >= icalcomponent_get_sequence(current))
                current = event;
        }
        for (const auto& [id, event] : overrides)
        {
            if (Cancelled(event)) continue;
            const auto appointment = ReadAppointment(calendar, event, master ? &*master : nullptr);
            Project(appointment, appointment.start, uid, std::to_string(id.time_since_epoch().count()));
        }
        if (!master) return;

        auto* event = group.master;
        auto* rule = icalcomponent_get_first_property(event, ICAL_RRULE_PROPERTY);
        auto* rdate = icalcomponent_get_first_property(event, ICAL_RDATE_PROPERTY);
        if (!rule && !rdate && overrides.empty())
        {
            Project(*master, master->start, uid, {});
            return;
        }

        std::unordered_set<long long> emitted;
        for (auto* date = icalcomponent_get_first_property(event, ICAL_EXDATE_PROPERTY); date;
            date = icalcomponent_get_next_property(event, ICAL_EXDATE_PROPERTY))
        {
            emitted.insert(Timeline(ReadTime(calendar, date, icalproperty_get_exdate(date),
                defaultZone)).time_since_epoch().count());
        }
        const auto emit = [&](const Appointment& appointment, EventTime start) {
            const auto stamp = Timeline(start);
            const auto id = stamp.time_since_epoch().count();
            if (!overrides.contains(stamp) && emitted.insert(id).second)
                Project(appointment, start, uid, std::to_string(id));
        };
        for (; rdate; rdate = icalcomponent_get_next_property(event, ICAL_RDATE_PROPERTY))
        {
            const auto value = icalproperty_get_rdate(rdate);
            auto appointment = *master;
            if (!icaltime_is_null_time(value.time))
            {
                emit(appointment, ReadTime(calendar, rdate, value.time, defaultZone));
                continue;
            }
            const auto start = ReadTime(calendar, rdate, value.period.start, defaultZone);
            appointment.duration = icaltime_is_null_time(value.period.end)
                ? seconds{icaldurationtype_as_utc_seconds(value.period.duration)}
                : Timeline(ReadTime(calendar, rdate, value.period.end, defaultZone)) - Timeline(start);
            if (appointment.duration < seconds::zero()) throw InvalidFeed{};
            appointment.wallDuration = false;
            emit(appointment, start);
        }
        emit(*master, master->start);
        for (; rule; rule = icalcomponent_get_next_property(event, ICAL_RRULE_PROPERTY))
        {
            using Rule = std::unique_ptr<icalrecurrencetype, decltype(&icalrecurrencetype_unref)>;
            auto* original = icalproperty_get_rrule(rule);
            Rule recurrence(original ? icalrecurrencetype_clone(original) : nullptr, icalrecurrencetype_unref);
            if (!recurrence) throw InvalidFeed{};
            const auto until = recurrence->until;
            recurrence->until = icaltime_null_time();
            auto floatingStart = master->start.value;
            floatingStart.zone = nullptr;
            using Iterator = std::unique_ptr<icalrecur_iterator, decltype(&icalrecur_iterator_free)>;
            Iterator iterator(icalrecur_iterator_new(recurrence.get(), floatingStart), icalrecur_iterator_free);
            if (!iterator) throw InvalidFeed{};
            const Stamp earliest = first - days{2} - master->duration;
            if (recurrence->count == 0 && earliest > WallTime(floatingStart) &&
                !icalrecur_iterator_set_start(iterator.get(), ToIcal(earliest, floatingStart.is_date)))
                throw InvalidFeed{};
            for (auto next = icalrecur_iterator_next(iterator.get()); !icaltime_is_null_time(next);
                next = icalrecur_iterator_next(iterator.get()))
            {
                if (++iterationCount > MaximumOccurrences || token.stop_requested()) throw InvalidFeed{};
                const auto wall = WallTime(next);
                if (wall >= end + days{2}) break;
                const EventTime start{next, defaultZone};
                if (!icaltime_is_null_time(until) &&
                    (icaltime_is_utc(until) ? Timeline(start) : wall) > WallTime(until)) break;
                if (wall >= earliest) emit(*master, start);
            }
        }
    }
};
} // namespace

std::optional<std::vector<CalendarEvent>> ParseIcsEvents(const std::string& text,
    std::string_view sourceId, const std::string& today, std::stop_token token)
{
    const auto date = CalendarService::GetDateInfo(today);
    if (!date || text.find('\0') != std::string::npos || token.stop_requested()) return std::nullopt;
    const auto offset = text.starts_with("\xef\xbb\xbf") ? 3u : 0u;
    if (!HasCalendarEnvelope(std::string_view{text}.substr(offset))) return std::nullopt;
    icalproperty_set_allow_empty_properties(true);
    const std::unique_ptr<Component, decltype(&icalcomponent_free)> calendar(
        icalparser_parse_string(text.c_str() + offset), icalcomponent_free);
    if (!calendar || icalcomponent_isa(calendar.get()) != ICAL_VCALENDAR_COMPONENT ||
        icalcomponent_count_errors(calendar.get()) != 0)
        return std::nullopt;

    try
    {
        const sys_days center{year{date->year} / static_cast<unsigned>(date->month) / static_cast<unsigned>(date->day)};
        Expansion expansion{calendar.get(), sourceId, center - days{366}, center + days{731}, current_zone(), token, {}};
        std::map<std::string, Group> groups;
        for (auto* event = icalcomponent_get_first_component(calendar.get(), ICAL_VEVENT_COMPONENT);
            event; event = icalcomponent_get_next_component(calendar.get(), ICAL_VEVENT_COMPONENT))
        {
            const char* uid = icalcomponent_get_uid(event);
            if (!uid || !*uid || token.stop_requested()) return std::nullopt;
            auto& group = groups[uid];
            if (icalcomponent_get_first_property(event, ICAL_RECURRENCEID_PROPERTY))
                group.exceptions.push_back(event);
            else if (!group.master || icalcomponent_get_sequence(event) >= icalcomponent_get_sequence(group.master))
                group.master = event;
        }
        for (const auto& [uid, group] : groups)
            expansion.AddGroup(uid, group);
        std::sort(expansion.events.begin(), expansion.events.end(), [](const auto& a, const auto& b) {
            return a.date < b.date || (a.date == b.date && a.id < b.id);
        });
        return std::move(expansion.events);
    }
    catch (const InvalidFeed&)
    {
        return std::nullopt;
    }
    catch (const std::runtime_error&)
    {
        return std::nullopt;
    }
}

} // namespace snowdesktop::calendar
