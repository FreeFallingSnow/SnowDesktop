#pragma once

#include "dock_fullscreen_policy.h"
#include <span>
#include <string_view>
#include <utility>

struct DockTrayRule
{
    bool operator==(const DockTrayRule&) const = default;
    std::wstring executable;
    bool matchPath = false;
    bool enabled = true;
    bool doubleClick = true;
};

namespace snowdesktop::dock_tray
{
inline std::wstring ExecutableName(std::wstring_view path)
{
    const auto at = path.find_last_of(L"\\/");
    return std::wstring(at == std::wstring_view::npos ? path : path.substr(at + 1));
}

inline bool Equal(std::wstring_view a, std::wstring_view b)
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

inline std::wstring NormalizeTarget(std::wstring path, bool matchPath)
{
    if (matchPath) return dock_fullscreen::NormalizeExecutable(std::move(path));
    if (path.empty() || path.size() > 255 || path.find(L'\0') != std::wstring::npos ||
        path.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return {};
    const auto first = path.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    path = path.substr(first, path.find_last_not_of(L" \t\r\n") - first + 1);
    if (path.size() <= 4 || !Equal(std::wstring_view(path).substr(path.size() - 4), L".exe")) return {};
    return path;
}

inline std::vector<DockTrayRule> Defaults()
{
    return {{L"QQ.exe"}, {L"Weixin.exe"}, {L"WeChat.exe"}, {L"Feishu.exe"}};
}

inline bool Matches(const DockTrayRule& rule, std::wstring_view executable)
{
    return !executable.empty() && Equal(rule.executable,
        rule.matchPath ? executable : std::wstring_view(ExecutableName(executable)));
}

// A disabled path override still outranks a name rule. Removing/turning off
// user preferences must not silently reactivate a built-in default.
inline const DockTrayRule* FindRule(std::span<const DockTrayRule> rules, std::wstring_view executable)
{
    const DockTrayRule* result = nullptr;
    for (const auto& rule : rules)
        if (Matches(rule, executable) && (!result || (rule.matchPath && !result->matchPath))) result = &rule;
    return result;
}

inline void NormalizeRules(std::vector<DockTrayRule>& rules)
{
    std::vector<DockTrayRule> valid;
    for (auto rule : rules)
    {
        rule.executable = NormalizeTarget(std::move(rule.executable), rule.matchPath);
        if (rule.executable.empty()) continue;
        const auto duplicate = std::find_if(valid.begin(), valid.end(), [&](const auto& existing) {
            return existing.matchPath == rule.matchPath && Equal(existing.executable, rule.executable);
        });
        if (duplicate != valid.end()) *duplicate = std::move(rule);
        else if (valid.size() < 128) valid.push_back(std::move(rule));
    }
    rules = std::move(valid);
}
}
