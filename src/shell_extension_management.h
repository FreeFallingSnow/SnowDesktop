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
    unsigned contexts = 0;
};
inline int ManagementGroup(const ManagementRow &row, Category category)
{
    const unsigned mask = category == Category::Objects ? 3 : 12;
    const bool typed = !row.types.empty() && std::find(row.types.begin(), row.types.end(), L"*") == row.types.end();
    return typed ? 3 : (row.contexts & mask) == mask ? 0 : (row.contexts & (category == Category::Objects ? 1 : 4)) ? 1 : 2;
}
inline std::vector<ManagementRow> ManagementRows(const Catalogue &catalogue, Category category, std::wstring filter = {})
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
        row.types.insert(row.types.end(), entry.types.begin(), entry.types.end());
    }
    for (auto &c : filter) c = towlower(c);
    std::vector<ManagementRow> result;
    for (auto &[id, row] : grouped)
    {
        std::sort(row.members.begin(), row.members.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        std::sort(row.types.begin(), row.types.end());
        row.types.erase(std::unique(row.types.begin(), row.types.end()), row.types.end());
        auto text = row.display.label;
        for (const auto &type : row.types) text += L" " + type;
        for (auto &c : text) c = towlower(c);
        if (filter.empty() || text.find(filter) != std::wstring::npos) result.push_back(std::move(row));
    }
    std::stable_sort(result.begin(), result.end(), [category](const auto &a, const auto &b) {
        const auto x = ManagementGroup(a, category), y = ManagementGroup(b, category);
        return x != y ? x < y : a.display.label < b.display.label;
    });
    return result;
}
inline bool SameManagementRow(const ManagementRow &a, const ManagementRow &b)
{
    return a.id == b.id && a.contexts == b.contexts && a.types == b.types && a.members == b.members &&
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
    for (const auto &member : row.members) SetCommon(prefs, member.id, category, shown);
}
inline void SetManagementOverride(Preferences &prefs, const ManagementRow &row, Context context, Visibility value)
{
    for (const auto &member : row.members) if (member.contexts & ContextBit(context)) SetOverride(prefs, member.id, context, value);
}
} // namespace snowdesktop::shell_extensions
