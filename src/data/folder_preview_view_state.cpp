#include "folder_preview_view_state.h"

#include "common/json_value.h"
#include "common/utils.h"
#include "data/atomic_file.h"
#include "data/data_paths.h"

#include <filesystem>
#include <limits>
#include <sstream>

namespace snowdesktop::folder_preview
{
namespace
{
constexpr wchar_t kFilename[] = L"folder-preview-views.json";
}

void ViewPreferences::Load()
{
    if (loaded_) return;
    loaded_ = true;
    std::string contents;
    if (!atomic_file::ReadAll(GetDataFilePath(kFilename), contents)) return;
    JsonValue root;
    if (!ParseJson(contents, root) || !root.IsObject()) return;
    for (const auto& [key, value] : root.object)
    {
        const auto* pathValue = value.Find("path");
        if (key.empty() || !pathValue || !pathValue->IsString()) continue;
        auto path = Utf8ToWide(pathValue->string);
        if (!std::filesystem::path(path).is_absolute()) continue;
        SavedView view;
        if (const auto* source = value.Find("source"); source && source->IsString())
            view.sourceWidgetId = Utf8ToWide(source->string);
        if (const auto* list = value.Find("listMode"); list && list->IsBoolean()) view.listMode = list->boolean;
        if (const auto* dates = value.Find("dateHeaders"); dates && dates->IsBoolean()) view.dateHeaders = dates->boolean;
        const auto* offset = value.Find("scroll");
        const int scroll = offset && offset->IsNumber() && offset->number >= 0 &&
            offset->number <= (std::numeric_limits<int>::max)() ? static_cast<int>(offset->number) : 0;
        view.page = ViewPage{std::move(path), scroll};
        entries_.emplace(Utf8ToWide(key), std::move(view));
    }
}

std::optional<SavedView> ViewPreferences::Get(const std::wstring& key)
{
    Load();
    const auto found = entries_.find(key);
    if (found == entries_.end()) return std::nullopt;
    return found->second;
}

bool ViewPreferences::Set(const std::wstring& key, SavedView view)
{
    if (key.empty()) return false;
    Load();
    if (const auto found = entries_.find(key); found == entries_.end() ? !view.page : found->second == view)
        return true;
    auto updated = entries_;
    if (!view.page) updated.erase(key);
    else updated.insert_or_assign(key, std::move(view));
    std::ostringstream json;
    json << "{\n";
    bool first = true;
    for (const auto& [owner, stored] : updated)
    {
        if (!first) json << ",\n";
        first = false;
        const auto& page = *stored.page;
        json << "  \"" << JsonEscapeUtf8(owner) << "\": {\"path\": \"" << JsonEscapeUtf8(page.path)
             << "\", \"scroll\": " << page.scroll << ", \"source\": \"" << JsonEscapeUtf8(stored.sourceWidgetId)
             << "\", \"listMode\": " << (stored.listMode ? "true" : "false")
             << ", \"dateHeaders\": " << (stored.dateHeaders ? "true" : "false") << "}";
    }
    json << "\n}\n";
    if (!atomic_file::WriteAll(GetDataFilePath(kFilename), json.str())) return false;
    entries_ = std::move(updated);
    return true;
}
}
