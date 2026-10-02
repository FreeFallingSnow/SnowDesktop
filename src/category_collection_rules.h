#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::category_collection_rules
{
struct ShortcutTarget
{
    bool classified = false;
    bool directory = false;
    bool application = false;
    std::wstring extension;
};

inline std::wstring NormalizeExtensionToken(std::wstring token)
{
    while (!token.empty() && token.front() == L'*') token.erase(token.begin());
    if (!token.empty() && token.front() != L'.') token.insert(token.begin(), L'.');
    for (auto& ch : token) ch = static_cast<wchar_t>(std::towupper(ch));
    if (token.starts_with(L".LNK:"))
    {
        const auto target = token.substr(5);
        if (!target.empty() && target != L"FOLDER" && target != L"APP" && target.front() != L'.')
            token.insert(5, L".");
    }
    return token;
}

inline std::vector<std::wstring> MergeLegacyProgramShortcutRules(std::vector<std::wstring> tokens)
{
    for (auto& token : tokens)
    {
        if (token == L".LNK") token = L".LNK:APP";
        if (token == L".URL") token = L".URL:STEAM";
    }
    std::vector<std::wstring> merged;
    for (const auto& token : tokens)
        if (std::find(merged.begin(), merged.end(), token) == merged.end()) merged.push_back(token);
    for (const auto added : {L".LNK:APP", L".URL:STEAM"})
        if (std::find(merged.begin(), merged.end(), added) == merged.end()) merged.emplace_back(added);
    return merged;
}

inline bool MatchesExtensionRule(std::wstring_view rule, std::wstring_view extension,
    const ShortcutTarget& target = {})
{
    if (rule == L".LNK:FOLDER") return extension == L".LNK" && target.directory;
    if (rule == L".LNK:APP") return extension == L".LNK" && target.application;
    if (rule == L".URL:STEAM") return extension == L".URL" && target.application;
    if (rule.starts_with(L".LNK:."))
        return extension == L".LNK" && !target.directory && rule.substr(5) == target.extension;
    // Plain suffixes also match a link's file target. Explicit .LNK/.URL rules
    // retain their literal meaning when a user chooses to group all such links.
    return rule == extension || (extension == L".LNK" && !target.directory &&
        !target.extension.empty() && rule == target.extension);
}

inline bool IsProgramItem(std::wstring_view extension,
    bool applicationShortcut,
    const std::vector<std::wstring>& configuredExtensions,
    const ShortcutTarget& target = {})
{
    std::wstring upper(extension);
    for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
    if (target.directory) return false;
    const auto& effective = upper == L".LNK" ? target.extension : upper;
    // Removing a matching rule must not bypass the collection opt-in.
    return applicationShortcut || target.application || effective == L".EXE" ||
        effective == L".MSI" || effective == L".BAT" || effective == L".CMD" ||
        std::any_of(configuredExtensions.begin(), configuredExtensions.end(),
            [&](const std::wstring& rule) { return MatchesExtensionRule(rule, upper, target); });
}

inline std::vector<std::wstring> ResolveTabOrder(
    const std::vector<std::wstring>& defaults,
    const std::vector<std::wstring>& saved)
{
    std::vector<std::wstring> result;
    auto append = [&](const std::wstring& id) {
        if (std::find(defaults.begin(), defaults.end(), id) != defaults.end() &&
            std::find(result.begin(), result.end(), id) == result.end())
            result.push_back(id);
    };
    for (const auto& id : saved) append(id);
    for (const auto& id : defaults) append(id);
    return result;
}

inline bool MoveTab(std::vector<std::wstring>& order,
    const std::wstring& source, const std::wstring& target)
{
    const auto from = std::find(order.begin(), order.end(), source);
    const auto to = std::find(order.begin(), order.end(), target);
    if (from == order.end() || to == order.end() || from == to) return false;
    const auto destination = to - order.begin();
    const auto value = *from;
    order.erase(from);
    order.insert(order.begin() + destination, value);
    return true;
}
}
