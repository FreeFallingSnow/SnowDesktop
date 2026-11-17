#include "folder_preview_sort.h"

#include "common/json_value.h"
#include "common/utils.h"
#include "data/atomic_file.h"
#include "data/data_paths.h"
#include "layout/folder_sort_rules.h"

#include <sstream>

namespace snowdesktop::folder_preview
{
namespace
{
constexpr wchar_t kFilename[] = L"folder-preview-sort.json";
}

void SortPreferences::Load()
{
    if (loaded_) return;
    loaded_ = true;
    std::string contents;
    if (!atomic_file::ReadAll(GetDataFilePath(kFilename), contents)) return;
    JsonValue root;
    if (!ParseJson(contents, root) || !root.IsObject()) return;
    for (const auto& [path, value] : root.object)
    {
        const auto* mode = value.Find("mode");
        const auto* ascending = value.Find("ascending");
        if (path.empty() || !mode || !mode->IsNumber() || !ascending || !ascending->IsBoolean())
            continue;
        for (int column = folder_sort_rules::kManual; column <= folder_sort_rules::kSize; ++column)
        {
            if (mode->number != column) continue;
            Sort sort{column, ascending->boolean};
            if (column == folder_sort_rules::kManual)
                if (const auto* order = value.Find("order"); order && order->IsArray())
                    for (const auto& item : order->array)
                        if (item.IsString() && !item.string.empty()) sort.order.push_back(Utf8ToWide(item.string));
            entries_.emplace(Utf8ToWide(path), std::move(sort));
            break;
        }
    }
}

Sort SortPreferences::Get(const std::wstring& key)
{
    Load();
    const auto found = entries_.find(key);
    return found == entries_.end() ? Sort{folder_sort_rules::kName, true} : found->second;
}

bool SortPreferences::Set(const std::wstring& key, Sort sort)
{
    Load();
    if (const auto found = entries_.find(key); found != entries_.end() && found->second == sort)
        return true;
    auto updated = entries_;
    updated.insert_or_assign(key, std::move(sort));
    std::ostringstream json;
    json << "{\n";
    bool first = true;
    for (const auto& [path, preference] : updated)
    {
        if (!first) json << ",\n";
        first = false;
        json << "  \"" << JsonEscapeUtf8(path) << "\": { \"mode\": " << preference.mode
             << ", \"ascending\": " << (preference.ascending ? "true" : "false");
        if (preference.mode == folder_sort_rules::kManual)
        {
            json << ", \"order\": [";
            for (size_t index = 0; index < preference.order.size(); ++index)
            {
                if (index != 0) json << ", ";
                json << "\"" << JsonEscapeUtf8(preference.order[index]) << "\"";
            }
            json << "]";
        }
        json << " }";
    }
    json << "\n}\n";
    if (!atomic_file::WriteAll(GetDataFilePath(kFilename), json.str())) return false;
    entries_ = std::move(updated);
    return true;
}
}
