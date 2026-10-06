#pragma once
#include "shell_extension_catalogue.h"
#include <cwctype>

namespace snowdesktop::shell_extensions
{
struct ManagementMember
{
    std::string id;
    unsigned contexts = 0;
    friend bool operator==(const ManagementMember &, const ManagementMember &) = default;
};
struct ManagementRow
{
    std::string id;
    Entry display;
    std::vector<std::wstring> types;
    std::vector<ManagementMember> members;
    std::vector<Application> applications;
    unsigned contexts = 0;
};
inline int ManagementGroup(const ManagementRow &row, Category category)
{
    const unsigned mask = category == Category::Objects ? 3 : 12;
    const bool typed = !row.types.empty() && std::find(row.types.begin(), row.types.end(), L"*") == row.types.end();
    return typed ? 3 : (row.contexts & mask) == mask ? 0 : (row.contexts & (category == Category::Objects ? 1 : 4)) ? 1 : 2;
}
inline bool MatchesManagementFilters(const ManagementRow &row, const std::string &application, std::wstring extension)
{
    if (!application.empty() && std::none_of(row.applications.begin(), row.applications.end(), [&](const auto &app) {
        return application == "@unknown" ? app.id.empty() : app.id == application;
    })) return false;
    if (extension.empty()) return true;
    if (!(row.contexts & ContextBit(Context::File))) return false;
    for (auto &c : extension) c = towlower(c);
    if (extension != L"*" && extension.front() != L'.') extension.insert(extension.begin(), L'.');
    const bool common = row.types.empty() || std::find(row.types.begin(), row.types.end(), L"*") != row.types.end();
    return common || std::any_of(row.types.begin(), row.types.end(), [&](auto type) {
        for (auto &c : type) c = towlower(c); return type == extension;
    });
}
inline std::vector<ManagementRow> ManagementRows(const Catalogue &catalogue, Category category, std::wstring filter = {},
    const std::string &application = {}, const std::wstring &extension = {})
{
    std::map<std::string, ManagementRow> grouped;
    const unsigned mask = category == Category::Objects ? 3 : 12;
    for (const auto &entry : catalogue.rows)
    {
        if (!entry.systemEnabled || !entry.linked || !(entry.contexts & mask)) continue;
        const auto id = entry.commandIdentity.empty() ? entry.id : "command:" + entry.commandIdentity;
        auto [it, added] = grouped.try_emplace(id);
        auto &row = it->second;
        if (added) { row.id = id; row.display = entry.display; }
        else if (row.display.pixels.empty() && !entry.display.pixels.empty()) row.display = entry.display;
        row.members.push_back({entry.id, entry.contexts});
        row.contexts |= entry.contexts;
        if (std::find(row.applications.begin(), row.applications.end(), entry.application) == row.applications.end()) row.applications.push_back(entry.application);
        row.types.insert(row.types.end(), entry.types.begin(), entry.types.end());
    }
    for (auto &c : filter) c = towlower(c);
    std::vector<ManagementRow> result;
    for (auto &[id, row] : grouped)
    {
        std::sort(row.members.begin(), row.members.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        std::sort(row.types.begin(), row.types.end());
        std::sort(row.applications.begin(), row.applications.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        row.types.erase(std::unique(row.types.begin(), row.types.end()), row.types.end());
        auto text = row.display.label;
        for (const auto &type : row.types) text += L" " + type;
        for (const auto &app : row.applications) text += L" " + app.name;
        for (auto &c : text) c = towlower(c);
        if ((filter.empty() || text.find(filter) != std::wstring::npos) && MatchesManagementFilters(row, application, extension)) result.push_back(std::move(row));
    }
    std::stable_sort(result.begin(), result.end(), [category](const auto &a, const auto &b) {
        const auto x = ManagementGroup(a, category), y = ManagementGroup(b, category);
        return x != y ? x < y : a.display.label < b.display.label;
    });
    return result;
}
inline bool SameManagementRow(const ManagementRow &a, const ManagementRow &b)
{
    return a.id == b.id && a.contexts == b.contexts && a.types == b.types && a.members == b.members && a.applications == b.applications &&
        a.display.label == b.display.label && a.display.width == b.display.width &&
        a.display.height == b.display.height && a.display.pixels == b.display.pixels;
}
// Reconcile identities without clearing existing controls. Metadata revisions,
// child commands and execution tokens are intentionally irrelevant to settings.
template<class Rows, class Remove>
std::vector<ManagementRow> PlanManagementUpdates(Rows &current, const std::vector<ManagementRow> &desired, Remove remove)
{
    std::erase_if(current, [&](auto &row) {
        if (std::any_of(desired.begin(), desired.end(), [&](const auto &next) { return next.id == row.model.id; })) return false;
        remove(row); return true;
    });
    std::vector<ManagementRow> updates;
    for (const auto &next : desired)
    {
        const auto old = std::find_if(current.begin(), current.end(), [&](const auto &row) { return row.model.id == next.id; });
        if (old == current.end() || !SameManagementRow(old->model, next)) updates.push_back(next);
    }
    return updates;
}
inline std::optional<bool> ManagementCommon(const Preferences &prefs, const ManagementRow &row, Category category)
{
    std::optional<bool> result;
    for (const auto &member : row.members)
    {
        const bool shown = CommonShown(prefs, member.id, category);
        if (result && *result != shown) return {};
        result = shown;
    }
    return result;
}
inline std::optional<Visibility> ManagementOverride(const Preferences &prefs, const ManagementRow &row, Context context)
{
    std::optional<Visibility> result;
    for (const auto &member : row.members) if (member.contexts & ContextBit(context))
    {
        const auto visibility = OverrideOf(prefs, member.id, context);
        if (result && *result != visibility) return {};
        result = visibility;
    }
    return result;
}
inline void SetManagementCommon(Preferences &prefs, const ManagementRow &row, Category category, bool shown)
{
    for (const auto &member : row.members)
    {
        // The primary switch is an explicit show/hide action. Old migrated
        // location opt-ins must not silently defeat it. Location controls can
        // add new exceptions afterwards; the other category is untouched.
        std::erase_if(prefs.overrides, [&](const auto &r) { return r.id == member.id && CategoryOf(r.context) == category; });
        SetCommon(prefs, member.id, category, shown);
    }
}
inline void SetManagementResults(Preferences &prefs, const std::vector<ManagementRow> &results, Category category, bool shown)
{
    // Capture the displayed result set at the click. Never expand a batch to
    // future discovery results, another category, or a hidden registry record.
    for (const auto &row : results) SetManagementCommon(prefs, row, category, shown);
}
inline void SetManagementOverride(Preferences &prefs, const ManagementRow &row, Context context, Visibility value)
{
    for (const auto &member : row.members) if (member.contexts & ContextBit(context)) SetOverride(prefs, member.id, context, value);
}
enum class ManagementView { Objects, Applications, Extensions };
struct ManagementCard
{
    std::string id, titleKey;
    std::wstring title;
    std::vector<ManagementRow> rows;
};
inline std::vector<ManagementCard> ManagementCards(const std::vector<ManagementRow> &rows, Category category, ManagementView view)
{
    std::map<std::string, ManagementCard> cards;
    auto add = [&](std::string id, std::wstring title, std::string titleKey, const ManagementRow &row) {
        auto &card = cards[id]; card.id = id; card.title = std::move(title); card.titleKey = std::move(titleKey);
        auto display = row;
        // Only the UI instance identity changes; every view writes the same
        // original member IDs, so duplicated type cards cannot diverge.
        display.id = id + "\n" + row.id;
        if (std::none_of(card.rows.begin(), card.rows.end(), [&](const auto &existing) { return existing.id == display.id; })) card.rows.push_back(std::move(display));
    };
    for (const auto &row : rows)
        if (view == ManagementView::Applications)
        {
            if (row.applications.empty()) add("app:@unknown", {}, "settings.contextMenu.unknownApplication", row);
            for (const auto &app : row.applications)
                add("app:" + (app.id.empty() ? "@unknown" : app.id), app.name,
                    app.id.empty() ? "settings.contextMenu.unknownApplication" : "", row);
        }
        else if (view == ManagementView::Extensions && category == Category::Objects)
        {
            if (!(row.contexts & ContextBit(Context::File))) add("type:folder", {}, "settings.contextMenu.folders", row);
            else if (row.types.empty() || std::find(row.types.begin(), row.types.end(), L"*") != row.types.end())
                add("type:*", {}, "settings.contextMenu.anyExtension", row);
            else
            {
                bool added = false;
                for (auto type : row.types) if (!type.empty() && type.front() == L'.')
                {
                    for (auto &c : type) c = towlower(c);
                    const auto encoded = settings_ipc::Pack(type);
                    add("type:" + std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size()), type, {}, row); added = true;
                }
                if (!added) add("type:other", {}, "settings.contextMenu.types", row);
            }
        }
        else
        {
            const auto group = ManagementGroup(row, category);
            const char *key = group == 0 ? "settings.contextMenu.common" : group == 3 ? "settings.contextMenu.types" :
                category == Category::Objects ? (group == 1 ? "settings.contextMenu.files" : "settings.contextMenu.folders") :
                (group == 1 ? "settings.contextMenu.folderBackground" : "settings.contextMenu.desktop");
            add("object:" + std::to_string(group), {}, key, row);
        }
    std::vector<ManagementCard> result;
    for (auto &[id, card] : cards) result.push_back(std::move(card));
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        if (a.id == "app:@unknown" || b.id == "app:@unknown") return b.id == "app:@unknown" && a.id != b.id;
        return a.title < b.title;
    });
    return result;
}
inline std::optional<bool> ManagementCardState(const Preferences &prefs, const ManagementCard &card, Category category)
{
    std::optional<bool> state;
    for (const auto &row : card.rows)
    {
        const auto current = ManagementCommon(prefs, row, category);
        if (!current || (state && *state != *current)) return {};
        state = current;
    }
    return state.value_or(false);
}
} // namespace snowdesktop::shell_extensions
