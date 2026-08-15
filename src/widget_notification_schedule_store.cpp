#include "widget_notification_schedule_store.h"

#include "json_value.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr int SchemaVersion = 1;
constexpr double MaximumDueMs = 32503680000000.0; // 3000-01-01 UTC

bool IsValidUtf8(std::string_view value)
{
    for (std::size_t index = 0; index < value.size();)
    {
        const unsigned char first =
            static_cast<unsigned char>(value[index]);
        if (first <= 0x7F)
        {
            if (first == 0) return false;
            ++index;
            continue;
        }
        std::size_t count = 0;
        unsigned codePoint = 0;
        if ((first & 0xE0) == 0xC0)
        {
            count = 2;
            codePoint = first & 0x1F;
            if (codePoint < 2) return false;
        }
        else if ((first & 0xF0) == 0xE0)
        {
            count = 3;
            codePoint = first & 0x0F;
        }
        else if ((first & 0xF8) == 0xF0)
        {
            count = 4;
            codePoint = first & 0x07;
        }
        else
        {
            return false;
        }
        if (index + count > value.size()) return false;
        for (std::size_t offset = 1; offset < count; ++offset)
        {
            const unsigned char next =
                static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        if ((count == 3 && codePoint < 0x800) ||
            (count == 4 && codePoint < 0x10000) ||
            codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;
        index += count;
    }
    return true;
}

std::string EscapeJson(std::string_view value)
{
    std::string output;
    output.reserve(value.size() + 8);
    constexpr char Hex[] = "0123456789abcdef";
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                output += "\\u00";
                output.push_back(Hex[(ch >> 4) & 0x0F]);
                output.push_back(Hex[ch & 0x0F]);
            }
            else
            {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return output;
}

const JsonValue* StringField(const JsonValue& object, const char* name)
{
    const JsonValue* value = object.Find(name);
    return value && value->IsString() ? value : nullptr;
}
}

bool WidgetNotificationScheduleStore::Validate(
    const WidgetPersistedNotificationSchedule& entry,
    std::string& error)
{
    error.clear();
    if (entry.instanceId.empty() || entry.instanceId.size() > 256 ||
        entry.packageId.empty() || entry.packageId.size() > 128 ||
        entry.notificationId.empty() ||
            entry.notificationId.size() > 128 ||
        entry.title.empty() || entry.title.size() > 256 ||
        entry.message.empty() || entry.message.size() > 2048 ||
        entry.dueMs <= 0)
    {
        error = "notification schedule fields are out of range";
        return false;
    }
    if (!IsValidUtf8(entry.instanceId) ||
        !IsValidUtf8(entry.packageId) ||
        !IsValidUtf8(entry.notificationId) ||
        !IsValidUtf8(entry.title) ||
        !IsValidUtf8(entry.message))
    {
        error = "notification schedule text is not valid UTF-8";
        return false;
    }
    return true;
}

bool WidgetNotificationScheduleStore::LoadText(
    std::string_view text, std::string& error)
{
    error.clear();
    if (text.size() > MaximumFileBytes)
    {
        error = "notification schedule file exceeds size limit";
        return false;
    }
    JsonValue root;
    if (!ParseJson(text, root, &error) || !root.IsObject())
    {
        if (error.empty()) error = "notification schedule root is invalid";
        return false;
    }
    const JsonValue* schema = root.Find("schemaVersion");
    const JsonValue* entries = root.Find("entries");
    if (!schema || !schema->IsNumber() ||
        schema->number != SchemaVersion ||
        !entries || !entries->IsArray() ||
        entries->array.size() > MaximumEntries)
    {
        error = "notification schedule schema is invalid";
        return false;
    }

    std::vector<WidgetPersistedNotificationSchedule> loaded;
    std::unordered_set<std::string> ids;
    loaded.reserve(entries->array.size());
    for (const JsonValue& value : entries->array)
    {
        if (!value.IsObject())
        {
            error = "notification schedule entry is invalid";
            return false;
        }
        const JsonValue* instanceId = StringField(value, "instanceId");
        const JsonValue* packageId = StringField(value, "packageId");
        const JsonValue* notificationId =
            StringField(value, "notificationId");
        const JsonValue* title = StringField(value, "title");
        const JsonValue* message = StringField(value, "message");
        const JsonValue* dueMs = value.Find("dueMs");
        if (!instanceId || !packageId || !notificationId || !title ||
            !message || !dueMs || !dueMs->IsNumber() ||
            !std::isfinite(dueMs->number) ||
            std::trunc(dueMs->number) != dueMs->number ||
            dueMs->number <= 0 ||
            dueMs->number > MaximumDueMs)
        {
            error = "notification schedule entry fields are invalid";
            return false;
        }
        WidgetPersistedNotificationSchedule entry{
            instanceId->string, packageId->string,
            notificationId->string, title->string, message->string,
            static_cast<std::int64_t>(dueMs->number) };
        if (!Validate(entry, error) ||
            !ids.insert(entry.notificationId).second)
        {
            if (error.empty())
                error = "notification schedule IDs must be unique";
            return false;
        }
        loaded.push_back(std::move(entry));
    }
    entries_ = std::move(loaded);
    return true;
}

std::string WidgetNotificationScheduleStore::Serialize() const
{
    std::vector<WidgetPersistedNotificationSchedule> sorted = entries_;
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& left, const auto& right) {
            if (left.dueMs != right.dueMs) return left.dueMs < right.dueMs;
            return left.notificationId < right.notificationId;
        });
    std::ostringstream output;
    output << "{\n  \"schemaVersion\": " << SchemaVersion
        << ",\n  \"entries\": [";
    for (std::size_t index = 0; index < sorted.size(); ++index)
    {
        const auto& entry = sorted[index];
        output << (index == 0 ? "\n" : ",\n")
            << "    {\"instanceId\":\"" << EscapeJson(entry.instanceId)
            << "\",\"packageId\":\"" << EscapeJson(entry.packageId)
            << "\",\"notificationId\":\""
            << EscapeJson(entry.notificationId)
            << "\",\"title\":\"" << EscapeJson(entry.title)
            << "\",\"message\":\"" << EscapeJson(entry.message)
            << "\",\"dueMs\":" << entry.dueMs << '}';
    }
    if (!sorted.empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

bool WidgetNotificationScheduleStore::Upsert(
    WidgetPersistedNotificationSchedule entry, std::string& error)
{
    if (!Validate(entry, error)) return false;
    const auto sameId = std::find_if(entries_.begin(), entries_.end(),
        [&entry](const auto& candidate) {
            return candidate.notificationId == entry.notificationId;
        });
    if (sameId != entries_.end())
    {
        if (sameId->instanceId != entry.instanceId ||
            sameId->packageId != entry.packageId)
        {
            error = "notification schedule ID belongs to another owner";
            return false;
        }
        *sameId = std::move(entry);
        return true;
    }
    if (entries_.size() >= MaximumEntries)
    {
        error = "notification schedule store is full";
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

bool WidgetNotificationScheduleStore::Remove(
    std::string_view instanceId, std::string_view notificationId)
{
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [instanceId, notificationId](const auto& entry) {
            return entry.instanceId == instanceId &&
                entry.notificationId == notificationId;
        });
    if (found == entries_.end()) return false;
    entries_.erase(found);
    return true;
}

std::size_t WidgetNotificationScheduleStore::RemoveInstance(
    std::string_view instanceId)
{
    const std::size_t previous = entries_.size();
    std::erase_if(entries_, [instanceId](const auto& entry) {
        return entry.instanceId == instanceId;
    });
    return previous - entries_.size();
}

bool WidgetNotificationScheduleStore::UpdateText(
    std::string_view instanceId, std::string_view notificationId,
    const std::optional<std::string>& title,
    const std::optional<std::string>& message)
{
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [instanceId, notificationId](const auto& entry) {
            return entry.instanceId == instanceId &&
                entry.notificationId == notificationId;
        });
    if (found == entries_.end()) return false;
    WidgetPersistedNotificationSchedule updated = *found;
    if (title) updated.title = *title;
    if (message) updated.message = *message;
    std::string error;
    if (!Validate(updated, error)) return false;
    *found = std::move(updated);
    return true;
}

std::optional<WidgetPersistedNotificationSchedule>
WidgetNotificationScheduleStore::Find(
    std::string_view instanceId, std::string_view notificationId) const
{
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [instanceId, notificationId](const auto& entry) {
            return entry.instanceId == instanceId &&
                entry.notificationId == notificationId;
        });
    return found == entries_.end()
        ? std::nullopt : std::optional(*found);
}

std::vector<WidgetPersistedNotificationSchedule>
WidgetNotificationScheduleStore::ForInstance(
    std::string_view instanceId, std::string_view packageId) const
{
    std::vector<WidgetPersistedNotificationSchedule> result;
    for (const auto& entry : entries_)
    {
        if (entry.instanceId == instanceId &&
            entry.packageId == packageId)
            result.push_back(entry);
    }
    return result;
}

std::vector<WidgetPersistedNotificationSchedule>
WidgetNotificationScheduleStore::Due(std::int64_t nowMs) const
{
    std::vector<WidgetPersistedNotificationSchedule> result;
    for (const auto& entry : entries_)
    {
        if (entry.dueMs <= nowMs) result.push_back(entry);
    }
    return result;
}
}
