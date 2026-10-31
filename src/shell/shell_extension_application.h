#pragma once
#include "shell_extension_catalogue.h"
#include "shell_extension_display_name.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace snowdesktop::shell_extensions
{
inline std::wstring ApplicationPathKey(std::wstring path)
{
    for (auto& c : path) { if (c == L'/') c = L'\\'; c = towlower(c); }
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}
inline bool ApplicationPathContains(const std::wstring& root, const std::wstring& path)
{
    return path.size() > root.size() && path.starts_with(root) && path[root.size()] == L'\\';
}
inline std::wstring ApplicationDisplayName(std::wstring name)
{
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    // These are architecture qualifiers, not distinct installed products.
    for (const auto* suffix : {L" (x64)", L"(x64)", L" x64", L" (x86)", L"(x86)", L" (64-bit)", L" (64 bit)"})
    {
        const auto length = wcslen(suffix);
        if (name.size() >= length && ApplicationPathKey(name.substr(name.size() - length)) == suffix)
        { name.resize(name.size() - length); break; }
    }
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    return name;
}
struct InstalledApplicationRecord
{
    std::wstring root, name, icon;
    unsigned strength = 0;
};
struct ClassifiedApplication
{
    Application application;
    std::wstring icon;
};
// Installation evidence is presentation metadata. Original registration IDs
// remain the authority for menu routing, visibility and execution.
class ApplicationProfileIndex
{
public:
    void Add(InstalledApplicationRecord record)
    {
        record.root = ApplicationPathKey(std::move(record.root));
        record.name = ApplicationDisplayName(std::move(record.name));
        if (record.root.size() <= 3 || !UsableShellDisplayName(record.name)) return;
        profiles_[record.root].push_back(std::move(record));
    }
    std::optional<InstalledApplicationRecord> Find(const std::wstring& module) const
    {
        const auto path = ApplicationPathKey(module);
        std::optional<InstalledApplicationRecord> result;
        for (const auto& [root, profiles] : profiles_)
        {
            if (!ApplicationPathContains(root, path) || (result && root.size() <= result->root.size())) continue;
            const auto strength = std::max_element(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) { return a.strength < b.strength; })->strength;
            std::set<std::wstring> names;
            const InstalledApplicationRecord* selected = nullptr;
            for (const auto& profile : profiles) if (profile.strength == strength)
            {
                names.insert(ApplicationPathKey(profile.name));
                if (!selected || (selected->icon.empty() && !profile.icon.empty())) selected = &profile;
            }
            // Broad shared install locations (e.g. one custom parent used by
            // multiple Adobe products) never collapse their independent apps.
            if (names.size() == 1 && selected) result = *selected;
        }
        return result;
    }
    std::wstring SharedIcon(const std::wstring& module) const
    {
        const auto path = ApplicationPathKey(module);
        std::wstring result; size_t matched = 0;
        for (const auto& [root, profiles] : profiles_)
        {
            if (root.size() <= matched || !ApplicationPathContains(root, path)) continue;
            const auto strength = std::max_element(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) { return a.strength < b.strength; })->strength;
            if (strength < 20) continue;
            std::set<std::wstring> names, icons;
            bool complete = true;
            for (const auto& profile : profiles) if (profile.strength == strength)
            {
                names.insert(ApplicationPathKey(profile.name));
                if (profile.icon.empty()) complete = false;
                else icons.insert(profile.icon);
            }
            // Conflicting suite names may still agree on a shared brand icon.
            if (complete && names.size() > 1 && icons.size() == 1)
            { result = *icons.begin(); matched = root.size(); }
        }
        return result;
    }
private:
    std::map<std::wstring, std::vector<InstalledApplicationRecord>> profiles_;
};
}
