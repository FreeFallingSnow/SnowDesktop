#include "dock_usage_storage.h"

#include "common/json_value.h"
#include "common/utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace snowdesktop::dock_usage
{
void Load(const std::filesystem::path& path, Records& records)
{
    records.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    std::ostringstream stream;
    stream << file.rdbuf();
    JsonValue root;
    if (!ParseJson(stream.str(), root) || !root.IsObject()) return;
    const JsonValue* entries = root.Find("entries");
    if (!entries || !entries->IsArray()) return;

    auto readInteger = [](const JsonValue& object,
        std::string_view name, int& output)
    {
        const JsonValue* value = object.Find(name);
        if (!value || !value->IsNumber() ||
            !std::isfinite(value->number) ||
            std::trunc(value->number) != value->number ||
            value->number < std::numeric_limits<int>::min() ||
            value->number > std::numeric_limits<int>::max())
        {
            return false;
        }
        output = static_cast<int>(value->number);
        return true;
    };

    for (const JsonValue& entry : entries->array)
    {
        if (!entry.IsObject()) continue;
        const JsonValue* key = entry.Find("key");
        int launchCount = 0;
        int lastUsed = 0;
        if (key && key->IsString() &&
            readInteger(entry, "launchCount", launchCount) &&
            launchCount > 0)
        {
            readInteger(entry, "lastUsed", lastUsed);
            const std::wstring normalizedKey =
                ToUpperInvariant(Utf8ToWide(key->string));
            if (!normalizedKey.empty())
                records[normalizedKey] =
                    { launchCount, std::max(0, lastUsed) };
        }
    }
}

void Save(const std::filesystem::path& path, const Records& records,
    const std::function<bool(const std::wstring&)>& eligible)
{
    std::ofstream file(path,
        std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "{\n  \"entries\": [\n";
    size_t written = 0;
    for (const auto& [key, record] : records)
    {
        if (key.empty() || record.launchCount <= 0) continue;
        if (!eligible(key))
            continue;
        if (written++ > 0) file << ",\n";
        file << "    { \"key\": \"" << JsonEscapeUtf8(key)
             << "\", \"launchCount\": " << record.launchCount
             << ", \"lastUsed\": " << record.lastUsed << " }";
    }
    file << "\n  ]\n}\n";
}
}
